/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
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

#if ENABLE(JIT)
#include "JIT.h"

#include "BaselineJITRegisters.h"
#include "BytecodeStructs.h"
#include "CacheableIdentifierInlines.h"
#include "CommonSlowPaths.h"
#include "JITInlines.h"
#include "JITThunks.h"
#include "PyObjects.h"
#include "PyTuple.h"
#include "PythonOperators.h"
#include "SlowPathCall.h"
#include "ThunkGenerators.h"

namespace JSC {

// Python's opcodes. See python/README.md.
//
// What is done here is what is done to numbers, and to the built-in things that a program has most to do with. It is done as TaggedArithmetic.h and PythonOperators.cpp say, which are the definition. Whatever is not
// for here is for the slow path, which is those.

// ---- Numbers

// An int that is not a BigInt.
JIT::JumpList JIT::branchIfNotPythonSmallInt(VirtualRegister operand, GPRReg gpr)
{
    JumpList result;
    if (isOperandConstantInt(operand) && getConstantOperand(operand).isPlainInt32())
        return result;
    result.append(branchIfNotInt32(gpr));
    result.append(branchIfInt32IsWholeFloat(gpr));
    return result;
}

// The value of a number, whichever kind it is. That is all that comparing and dividing go by.
void JIT::unboxPythonNumber(GPRReg gpr, GPRReg scratchGPR, FPRReg fpr, JumpList& notNumber)
{
    Jump isInt32 = branchIfInt32(gpr);
    notNumber.append(branchIfNotNumber(gpr));
    unboxDoubleWithoutAssertions(gpr, scratchGPR, fpr);
    Jump done = jump();
    isInt32.link(this);
    convertInt32ToDouble(gpr, fpr);
    done.link(this);
}

// The values of two numbers of which at least one is a float, so that what comes of them is one. A double that JavaScript made can have the value of an int32, and then it is an int.
void JIT::unboxPythonNumbersIfEitherIsFloat(GPRReg leftGPR, GPRReg rightGPR, GPRReg scratchGPR, FPRReg leftFPR, FPRReg rightFPR, FPRReg scratchFPR, JumpList& otherwise)
{
    JumpList leftIsFloat;
    JumpList done;

    Jump leftIsInt32 = branchIfInt32(leftGPR);
    otherwise.append(branchIfNotNumber(leftGPR));
    unboxDoubleWithoutAssertions(leftGPR, scratchGPR, leftFPR);
    leftIsFloat.append(branchIfDoubleIsNotTaggedInteger(leftFPR, scratchGPR, scratchFPR));
    otherwise.append(jump());

    leftIsInt32.link(this);
    convertInt32ToDouble(leftGPR, leftFPR);
    leftIsFloat.append(branchIfInt32IsWholeFloat(leftGPR));

    // The left is an int.
    Jump rightIsInt32 = branchIfInt32(rightGPR);
    otherwise.append(branchIfNotNumber(rightGPR));
    unboxDoubleWithoutAssertions(rightGPR, scratchGPR, rightFPR);
    done.append(branchIfDoubleIsNotTaggedInteger(rightFPR, scratchGPR, scratchFPR));
    otherwise.append(jump());
    rightIsInt32.link(this);
    otherwise.append(branchIfInt32IsPlain(rightGPR));
    convertInt32ToDouble(rightGPR, rightFPR);
    done.append(jump());

    leftIsFloat.link(this);
    unboxPythonNumber(rightGPR, scratchGPR, rightFPR, otherwise);
    done.link(this);
}

static bool isDoneInBaseline(Python::BinaryOperator op)
{
    using Operator = Python::BinaryOperator;
    switch (op) {
    case Operator::Add:
    case Operator::Sub:
    case Operator::Mult:
    case Operator::Div:
    case Operator::LShift:
    case Operator::RShift:
    case Operator::BitOr:
    case Operator::BitXor:
    case Operator::BitAnd:
        return true;
    case Operator::Mod:
    case Operator::FloorDiv:
#if CPU(ARM64) || CPU(X86_64)
        return true;
#else
        return false;
#endif
    case Operator::MatMult:
    case Operator::Pow:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void JIT::emit_op_py_binary_op(const JSInstruction* currentInstruction)
{
    using Operator = Python::BinaryOperator;
    auto bytecode = currentInstruction->as<OpPyBinaryOp>();
    // To a number, x += y is x = x + y.
    auto op = static_cast<Operator>(bytecode.m_operation & ~Python::inPlaceOperatorFlag);
    if (!isDoneInBaseline(op)) {
        JITSlowPathCall slowPathCall(this, slow_path_py_binary_op);
        slowPathCall.call();
        return;
    }

    constexpr GPRReg leftGPR = regT0;
    constexpr GPRReg rightGPR = regT1;
    constexpr GPRReg resultGPR = regT2;
    constexpr GPRReg scratchGPR = regT3;

    emitGetVirtualRegister(bytecode.m_lhs, leftGPR);
    emitGetVirtualRegister(bytecode.m_rhs, rightGPR);

    if (op == Operator::Div) {
        // A float, whatever they are.
        JumpList slow;
        unboxPythonNumber(leftGPR, scratchGPR, fpRegT0, slow);
        unboxPythonNumber(rightGPR, scratchGPR, fpRegT1, slow);
        moveZeroToDouble(fpRegT2);
        slow.append(branchDouble(DoubleEqualAndOrdered, fpRegT1, fpRegT2));
        addSlowCase(slow);
        divDouble(fpRegT1, fpRegT0);
        boxTaggedFloat(fpRegT0, resultGPR, scratchGPR, fpRegT2);
        emitValueProfilingSite(bytecode, resultGPR);
        emitPutVirtualRegister(bytecode.m_dst, resultGPR);
        return;
    }

    addSlowCase(branchIfNotPythonSmallInt(bytecode.m_lhs, leftGPR));
    addSlowCase(branchIfNotPythonSmallInt(bytecode.m_rhs, rightGPR));

    switch (op) {
    case Operator::Add:
        addSlowCase(branchAdd32(Overflow, leftGPR, rightGPR, resultGPR));
        boxInt32(resultGPR, resultGPR);
        break;
    case Operator::Sub:
        addSlowCase(branchSub32(Overflow, leftGPR, rightGPR, resultGPR));
        boxInt32(resultGPR, resultGPR);
        break;
    case Operator::Mult:
        addSlowCase(branchMul32(Overflow, leftGPR, rightGPR, resultGPR));
        boxInt32(resultGPR, resultGPR);
        break;
    case Operator::BitAnd:
        and64(leftGPR, rightGPR, resultGPR);
        break;
    case Operator::BitOr:
        or64(leftGPR, rightGPR, resultGPR);
        break;
    case Operator::BitXor:
        xor32(leftGPR, rightGPR, resultGPR);
        boxInt32(resultGPR, resultGPR);
        break;
    case Operator::LShift:
        // By less than nothing is an error, and nothing that is shifted out is lost.
        addSlowCase(branch32(AboveOrEqual, rightGPR, TrustedImm32(32)));
        lshift32(leftGPR, rightGPR, resultGPR);
        rshift32(resultGPR, rightGPR, scratchGPR);
        addSlowCase(branch32(NotEqual, scratchGPR, leftGPR));
        boxInt32(resultGPR, resultGPR);
        break;
    case Operator::RShift:
        addSlowCase(branch32(AboveOrEqual, rightGPR, TrustedImm32(32)));
        rshift32(leftGPR, rightGPR, resultGPR);
        boxInt32(resultGPR, resultGPR);
        break;
    case Operator::Mod:
    case Operator::FloorDiv: {
#if CPU(ARM64) || CPU(X86_64)
        // By 0 is an error. By -1 is what can be too big.
        add32(TrustedImm32(1), rightGPR, scratchGPR);
        addSlowCase(branch32(BelowOrEqual, scratchGPR, TrustedImm32(1)));
        constexpr GPRReg quotientGPR = regT4;
        constexpr GPRReg remainderGPR = regT2;
#if CPU(ARM64)
        div32(leftGPR, rightGPR, quotientGPR);
        multiplySub32(quotientGPR, rightGPR, leftGPR, remainderGPR);
#else
        static_assert(leftGPR == X86Registers::eax && remainderGPR == X86Registers::edx);
        x86ConvertToDoubleWord32();
        x86Div32(rightGPR);
        move(X86Registers::eax, quotientGPR);
#endif
        // The machine rounds toward 0, and Python down: the remainder has the sign of what it is divided by.
        Jump isExact = branchTest32(Zero, remainderGPR);
        xor32(remainderGPR, rightGPR, scratchGPR);
        Jump hasSameSign = branch32(GreaterThanOrEqual, scratchGPR, TrustedImm32(0));
        sub32(TrustedImm32(1), quotientGPR);
        add32(rightGPR, remainderGPR);
        hasSameSign.link(this);
        isExact.link(this);
        boxInt32(op == Operator::Mod ? remainderGPR : quotientGPR, resultGPR);
#endif
        break;
    }
    case Operator::Div:
    case Operator::MatMult:
    case Operator::Pow:
        RELEASE_ASSERT_NOT_REACHED();
    }

    emitValueProfilingSite(bytecode, resultGPR);
    emitPutVirtualRegister(bytecode.m_dst, resultGPR);
}

void JIT::emitSlow_op_py_binary_op(const JSInstruction* currentInstruction, Vector<SlowCaseEntry>::iterator& iter)
{
    using Operator = Python::BinaryOperator;
    auto bytecode = currentInstruction->as<OpPyBinaryOp>();
    auto op = static_cast<Operator>(bytecode.m_operation & ~Python::inPlaceOperatorFlag);
    linkAllSlowCases(iter);

    if (op != Operator::Add && op != Operator::Sub && op != Operator::Mult) {
        JITSlowPathCall slowPathCall(this, slow_path_py_binary_op);
        slowPathCall.call();
        return;
    }

    // Floats. The operands are where they were.
    constexpr GPRReg leftGPR = regT0;
    constexpr GPRReg rightGPR = regT1;
    constexpr GPRReg resultGPR = regT2;
    constexpr GPRReg scratchGPR = regT3;
    JumpList otherwise;
    unboxPythonNumbersIfEitherIsFloat(leftGPR, rightGPR, scratchGPR, fpRegT0, fpRegT1, fpRegT2, otherwise);
    if (op == Operator::Add)
        addDouble(fpRegT1, fpRegT0);
    else if (op == Operator::Sub)
        subDouble(fpRegT1, fpRegT0);
    else
        mulDouble(fpRegT1, fpRegT0);
    boxTaggedFloat(fpRegT0, resultGPR, scratchGPR, fpRegT2);
    emitValueProfilingSite(bytecode, resultGPR);
    emitPutVirtualRegister(bytecode.m_dst, resultGPR);
    Jump done = jump();

    otherwise.link(this);
    JITSlowPathCall slowPathCall(this, slow_path_py_binary_op);
    slowPathCall.call();
    done.link(this);
}

void JIT::emit_op_py_unary_op(const JSInstruction* currentInstruction)
{
    using Operator = Python::UnaryOperator;
    auto bytecode = currentInstruction->as<OpPyUnaryOp>();
    auto op = static_cast<Operator>(bytecode.m_operation);
    constexpr GPRReg operandGPR = regT0;
    constexpr GPRReg resultGPR = regT2;
    constexpr GPRReg scratchGPR = regT3;

    emitGetVirtualRegister(bytecode.m_operand, operandGPR);
    if (op == Operator::Not) {
        addSlowCase(branchIfNotBoolean(operandGPR, scratchGPR));
        xor64(TrustedImm32(1), operandGPR, resultGPR);
    } else {
        addSlowCase(branchIfNotPythonSmallInt(bytecode.m_operand, operandGPR));
        switch (op) {
        case Operator::USub:
            move(operandGPR, resultGPR);
            addSlowCase(branchNeg32(Overflow, resultGPR));
            boxInt32(resultGPR, resultGPR);
            break;
        case Operator::Invert:
            not32(operandGPR, resultGPR);
            boxInt32(resultGPR, resultGPR);
            break;
        case Operator::UAdd:
            move(operandGPR, resultGPR);
            break;
        case Operator::Not:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }
    emitValueProfilingSite(bytecode, resultGPR);
    emitPutVirtualRegister(bytecode.m_dst, resultGPR);
}

static MacroAssembler::RelationalCondition int32ConditionFor(Python::ComparisonOperator op)
{
    using Operator = Python::ComparisonOperator;
    switch (op) {
    case Operator::Eq:
        return MacroAssembler::Equal;
    case Operator::NotEq:
        return MacroAssembler::NotEqual;
    case Operator::Lt:
        return MacroAssembler::LessThan;
    case Operator::LtE:
        return MacroAssembler::LessThanOrEqual;
    case Operator::Gt:
        return MacroAssembler::GreaterThan;
    case Operator::GtE:
        return MacroAssembler::GreaterThanOrEqual;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

// A NaN is not equal to anything, and neither more nor less.
static MacroAssembler::DoubleCondition doubleConditionFor(Python::ComparisonOperator op)
{
    using Operator = Python::ComparisonOperator;
    switch (op) {
    case Operator::Eq:
        return MacroAssembler::DoubleEqualAndOrdered;
    case Operator::NotEq:
        return MacroAssembler::DoubleNotEqualOrUnordered;
    case Operator::Lt:
        return MacroAssembler::DoubleLessThanAndOrdered;
    case Operator::LtE:
        return MacroAssembler::DoubleLessThanOrEqualAndOrdered;
    case Operator::Gt:
        return MacroAssembler::DoubleGreaterThanAndOrdered;
    case Operator::GtE:
        return MacroAssembler::DoubleGreaterThanOrEqualAndOrdered;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

void JIT::emit_op_py_compare_op(const JSInstruction* currentInstruction)
{
    using Operator = Python::ComparisonOperator;
    auto bytecode = currentInstruction->as<OpPyCompareOp>();
    auto op = static_cast<Operator>(bytecode.m_operation);
    constexpr GPRReg leftGPR = regT0;
    constexpr GPRReg rightGPR = regT1;
    constexpr GPRReg resultGPR = regT2;
    constexpr GPRReg scratchGPR = regT3;

    switch (op) {
    case Operator::In:
    case Operator::NotIn:
    case Operator::ExceptionMatch: {
        JITSlowPathCall slowPathCall(this, slow_path_py_compare_op);
        slowPathCall.call();
        return;
    }
    case Operator::Is:
    case Operator::IsNot: {
        emitGetVirtualRegister(bytecode.m_lhs, leftGPR);
        if (bytecode.m_rhs.isConstant() && m_unlinkedCodeBlock->constantSourceCodeRepresentation(bytecode.m_rhs) != SourceCodeRepresentation::LinkTimeConstant && getConstantOperand(bytecode.m_rhs).isUndefined()) {
            // x is None. JavaScript's null is None too.
            and64(TrustedImm32(~JSValue::UndefinedTag), leftGPR, scratchGPR);
            compare64(op == Operator::Is ? Equal : NotEqual, scratchGPR, TrustedImm32(JSValue::ValueNull), resultGPR);
        } else {
            emitGetVirtualRegister(bytecode.m_rhs, rightGPR);
            // Two cells are the same one or they are not. Two numbers can be the same though they are not written the same, and so can two Nones.
            or64(leftGPR, rightGPR, scratchGPR);
            addSlowCase(branchIfNotCell(scratchGPR));
            compare64(op == Operator::Is ? Equal : NotEqual, leftGPR, rightGPR, resultGPR);
        }
        break;
    }
    default:
        emitGetVirtualRegister(bytecode.m_lhs, leftGPR);
        emitGetVirtualRegister(bytecode.m_rhs, rightGPR);
        // Which of them is a float makes no difference to which is the greater.
        if (!isOperandConstantInt(bytecode.m_lhs))
            addSlowCase(branchIfNotInt32(leftGPR));
        if (!isOperandConstantInt(bytecode.m_rhs))
            addSlowCase(branchIfNotInt32(rightGPR));
        compare32(int32ConditionFor(op), leftGPR, rightGPR, resultGPR);
        break;
    }
    boxBoolean(resultGPR, resultGPR);
    emitValueProfilingSite(bytecode, resultGPR);
    emitPutVirtualRegister(bytecode.m_dst, resultGPR);
}

void JIT::emitSlow_op_py_compare_op(const JSInstruction* currentInstruction, Vector<SlowCaseEntry>::iterator& iter)
{
    using Operator = Python::ComparisonOperator;
    auto bytecode = currentInstruction->as<OpPyCompareOp>();
    auto op = static_cast<Operator>(bytecode.m_operation);
    linkAllSlowCases(iter);

    if (op == Operator::Is || op == Operator::IsNot) {
        JITSlowPathCall slowPathCall(this, slow_path_py_compare_op);
        slowPathCall.call();
        return;
    }

    constexpr GPRReg leftGPR = regT0;
    constexpr GPRReg rightGPR = regT1;
    constexpr GPRReg resultGPR = regT2;
    constexpr GPRReg scratchGPR = regT3;
    JumpList otherwise;
    unboxPythonNumber(leftGPR, scratchGPR, fpRegT0, otherwise);
    unboxPythonNumber(rightGPR, scratchGPR, fpRegT1, otherwise);
    compareDouble(doubleConditionFor(op), fpRegT0, fpRegT1, resultGPR);
    boxBoolean(resultGPR, resultGPR);
    emitValueProfilingSite(bytecode, resultGPR);
    emitPutVirtualRegister(bytecode.m_dst, resultGPR);
    Jump done = jump();

    otherwise.link(this);
    JITSlowPathCall slowPathCall(this, slow_path_py_compare_op);
    slowPathCall.call();
    done.link(this);
}

void JIT::emit_op_py_to_bool(const JSInstruction* currentInstruction)
{
    auto bytecode = currentInstruction->as<OpPyToBool>();
    constexpr GPRReg operandGPR = regT0;
    constexpr GPRReg scratchGPR = regT3;
    // It is what came of comparing, more often than not.
    emitGetVirtualRegister(bytecode.m_operand, operandGPR);
    addSlowCase(branchIfNotBoolean(operandGPR, scratchGPR));
    emitPutVirtualRegister(bytecode.m_dst, operandGPR);
}

void JIT::emitSlow_op_py_to_bool(const JSInstruction* currentInstruction, Vector<SlowCaseEntry>::iterator& iter)
{
    auto bytecode = currentInstruction->as<OpPyToBool>();
    constexpr GPRReg operandGPR = regT0;
    constexpr GPRReg resultGPR = regT2;
    constexpr GPRReg scratchGPR = regT3;
    linkAllSlowCases(iter);

    JumpList done;
    Jump isNotInt32 = branchIfNotInt32(operandGPR);
    compare32(NotEqual, operandGPR, TrustedImm32(0), resultGPR);
    boxBoolean(resultGPR, resultGPR);
    emitPutVirtualRegister(bytecode.m_dst, resultGPR);
    done.append(jump());

    isNotInt32.link(this);
    Jump isNotNone = branchIfNotOther(operandGPR, scratchGPR);
    move(TrustedImm32(JSValue::ValueFalse), resultGPR);
    emitPutVirtualRegister(bytecode.m_dst, resultGPR);
    done.append(jump());

    isNotNone.link(this);
    JITSlowPathCall slowPathCall(this, slow_path_py_to_bool);
    slowPathCall.call();
    done.link(this);
}

// ---- What is in a list or a tuple

// The item of a list that is at `indexGPR`, which is not negative. Only what is simply there to be read: JavaScript can make an array with a hole in it, or of doubles, or that is frozen, or has a getter for an element.
void JIT::loadPythonListItem(GPRReg listGPR, GPRReg indexGPR, GPRReg resultGPR, GPRReg scratchGPR, JumpList& outOfBounds, JumpList& slow)
{
    ASSERT(noOverlap(listGPR, indexGPR, resultGPR, scratchGPR));
    load8(Address(listGPR, JSCell::indexingTypeAndMiscOffset()), scratchGPR);
    and32(TrustedImm32(IndexingShapeMask), scratchGPR);
    Jump isContiguous = branch32(Equal, scratchGPR, TrustedImm32(ContiguousShape));
    slow.append(branch32(NotEqual, scratchGPR, TrustedImm32(Int32Shape)));
    isContiguous.link(this);
    loadPtr(Address(listGPR, JSObject::butterflyOffset()), scratchGPR);
    outOfBounds.append(branch32(AboveOrEqual, indexGPR, Address(scratchGPR, Butterfly::offsetOfPublicLength())));
    load64(BaseIndex(scratchGPR, indexGPR, TimesEight), resultGPR);
    slow.append(branchIfEmpty(resultGPR));
}

void JIT::emit_op_py_get_item(const JSInstruction* currentInstruction)
{
    auto bytecode = currentInstruction->as<OpPyGetItem>();
    constexpr GPRReg baseGPR = regT0;
    constexpr GPRReg keyGPR = regT1;
    constexpr GPRReg resultGPR = regT2;
    constexpr GPRReg scratchGPR = regT3;

    emitGetVirtualRegister(bytecode.m_base, baseGPR);
    emitGetVirtualRegister(bytecode.m_property, keyGPR);
    addSlowCase(branchIfNotCell(baseGPR));
    emitArrayProfilingSiteWithCell(bytecode, baseGPR, scratchGPR);
    addSlowCase(branchIfNotPythonSmallInt(bytecode.m_property, keyGPR));

    JumpList slow;
    Jump isNotList = branchIfNotType(baseGPR, ArrayType);
    {
        // From the end, if it is negative.
        Jump isFromTheStart = branch32(GreaterThanOrEqual, keyGPR, TrustedImm32(0));
        loadPtr(Address(baseGPR, JSObject::butterflyOffset()), scratchGPR);
        // An array with nothing in it may have nowhere to keep it, and then nothing is within it.
        slow.append(branchTestPtr(Zero, scratchGPR));
        add32(Address(scratchGPR, Butterfly::offsetOfPublicLength()), keyGPR);
        isFromTheStart.link(this);
        zeroExtend32ToWord(keyGPR, keyGPR);
        loadPythonListItem(baseGPR, keyGPR, resultGPR, scratchGPR, slow, slow);
    }
    Jump done = jump();

    isNotList.link(this);
    slow.append(branchIfNotType(baseGPR, PyTupleType));
    {
        Jump isFromTheStart = branch32(GreaterThanOrEqual, keyGPR, TrustedImm32(0));
        add32(Address(baseGPR, PyTuple::offsetOfLength()), keyGPR);
        isFromTheStart.link(this);
        zeroExtend32ToWord(keyGPR, keyGPR);
        slow.append(branch32(AboveOrEqual, keyGPR, Address(baseGPR, PyTuple::offsetOfLength())));
        load64(BaseIndex(baseGPR, keyGPR, TimesEight, PyTuple::offsetOfValues()), resultGPR);
    }
    addSlowCase(slow);

    done.link(this);
    emitValueProfilingSite(bytecode, resultGPR);
    emitPutVirtualRegister(bytecode.m_dst, resultGPR);
}

void JIT::emit_op_py_set_item(const JSInstruction* currentInstruction)
{
    auto bytecode = currentInstruction->as<OpPySetItem>();
    constexpr GPRReg baseGPR = regT0;
    constexpr GPRReg keyGPR = regT1;
    constexpr GPRReg valueGPR = regT2;
    constexpr GPRReg scratchGPR = regT3;

    emitGetVirtualRegister(bytecode.m_base, baseGPR);
    emitGetVirtualRegister(bytecode.m_property, keyGPR);
    addSlowCase(branchIfNotCell(baseGPR));
    emitArrayProfilingSiteWithCell(bytecode, baseGPR, scratchGPR);
    addSlowCase(branchIfNotPythonSmallInt(bytecode.m_property, keyGPR));
    addSlowCase(branchIfNotType(baseGPR, ArrayType));
    emitGetVirtualRegister(bytecode.m_value, valueGPR);

    // Not one that shares what is in it with the literal that it was made from. One that has had nothing but ints in it goes on that way only if this is one.
    load8(Address(baseGPR, JSCell::indexingTypeAndMiscOffset()), scratchGPR);
    and32(TrustedImm32(IndexingShapeAndWritabilityMask), scratchGPR);
    Jump isContiguous = branch32(Equal, scratchGPR, TrustedImm32(ContiguousShape));
    addSlowCase(branch32(NotEqual, scratchGPR, TrustedImm32(Int32Shape)));
    addSlowCase(branchIfNotPythonSmallInt(bytecode.m_value, valueGPR));
    isContiguous.link(this);

    loadPtr(Address(baseGPR, JSObject::butterflyOffset()), scratchGPR);
    Jump isFromTheStart = branch32(GreaterThanOrEqual, keyGPR, TrustedImm32(0));
    add32(Address(scratchGPR, Butterfly::offsetOfPublicLength()), keyGPR);
    isFromTheStart.link(this);
    zeroExtend32ToWord(keyGPR, keyGPR);
    addSlowCase(branch32(AboveOrEqual, keyGPR, Address(scratchGPR, Butterfly::offsetOfPublicLength())));
    store64(valueGPR, BaseIndex(scratchGPR, keyGPR, TimesEight));
    emitWriteBarrier(bytecode.m_base, bytecode.m_value, ShouldFilterValue);
}

void JIT::emit_op_py_iter_next(const JSInstruction* currentInstruction)
{
    using Kind = PyIterator::Kind;
    auto bytecode = currentInstruction->as<OpPyIterNext>();
    constexpr GPRReg iteratorGPR = regT0;
    constexpr GPRReg indexGPR = regT1;
    constexpr GPRReg resultGPR = regT2;
    constexpr GPRReg scratchGPR = regT3;
    constexpr GPRReg sequenceGPR = regT4;

    // Which way a loop goes is told of by the slow path.
    addSlowCase(branchTest32(Zero, AbsoluteAddress(vm().addressOfPythonLimitUnlessWatched())));
    emitGetVirtualRegister(bytecode.m_iterator, iteratorGPR);
    addSlowCase(branchIfNotCell(iteratorGPR));
    addSlowCase(branchIfNotType(iteratorGPR, PyIteratorType));
    load8(Address(iteratorGPR, PyIterator::offsetOfKind()), scratchGPR);
    load64(Address(iteratorGPR, PyIterator::offsetOfIndex()), indexGPR);

    JumpList done;
    JumpList slow;
    Jump isNotRange = branch32(NotEqual, scratchGPR, TrustedImm32(static_cast<int32_t>(Kind::Range)));
    {
        // `stop` is how many are left.
        load64(Address(iteratorGPR, PyIterator::offsetOfStop()), scratchGPR);
        Jump hasMore = branch64(GreaterThan, scratchGPR, TrustedImm32(0));
        move(TrustedImm64(JSValue::encode(JSValue())), resultGPR);
        done.append(jump());
        hasMore.link(this);
        signExtend32ToPtr(indexGPR, resultGPR);
        slow.append(branch64(NotEqual, resultGPR, indexGPR));
        sub64(TrustedImm32(1), scratchGPR);
        store64(scratchGPR, Address(iteratorGPR, PyIterator::offsetOfStop()));
        add64(Address(iteratorGPR, PyIterator::offsetOfStep()), indexGPR);
        store64(indexGPR, Address(iteratorGPR, PyIterator::offsetOfIndex()));
        zeroExtend32ToWord(resultGPR, resultGPR);
        boxInt32(resultGPR, resultGPR);
        done.append(jump());
    }

    // When one of these runs out it lets go of what it was going through, which is for the slow path. It has done that already if there is nothing here.
    isNotRange.link(this);
    load64(Address(iteratorGPR, PyIterator::offsetOfA()), sequenceGPR);
    slow.append(branchIfEmpty(sequenceGPR));
    slow.append(branch64(Above, indexGPR, TrustedImm32(std::numeric_limits<int32_t>::max())));
    Jump isNotList = branch32(NotEqual, scratchGPR, TrustedImm32(static_cast<int32_t>(Kind::List)));
    loadPythonListItem(sequenceGPR, indexGPR, resultGPR, scratchGPR, slow, slow);
    Jump gotItem = jump();

    isNotList.link(this);
    slow.append(branch32(NotEqual, scratchGPR, TrustedImm32(static_cast<int32_t>(Kind::Tuple))));
    slow.append(branch32(AboveOrEqual, indexGPR, Address(sequenceGPR, PyTuple::offsetOfLength())));
    load64(BaseIndex(sequenceGPR, indexGPR, TimesEight, PyTuple::offsetOfValues()), resultGPR);

    gotItem.link(this);
    add64(TrustedImm32(1), indexGPR);
    store64(indexGPR, Address(iteratorGPR, PyIterator::offsetOfIndex()));
    addSlowCase(slow);

    done.link(this);
    emitValueProfilingSite(bytecode, resultGPR);
    emitPutVirtualRegister(bytecode.m_dst, resultGPR);
}

void JIT::emit_op_py_unpack_sequence(const JSInstruction* currentInstruction)
{
    auto bytecode = currentInstruction->as<OpPyUnpackSequence>();
    constexpr GPRReg iterableGPR = regT0;
    constexpr GPRReg scratchGPR = regT3;
    // a, b = b, a makes a tuple and takes it apart.
    if (bytecode.m_starIndex != bytecode.m_argc) {
        JITSlowPathCall slowPathCall(this, slow_path_py_unpack_sequence);
        slowPathCall.call();
        return;
    }
    emitGetVirtualRegister(bytecode.m_iterable, iterableGPR);
    addSlowCase(branchIfNotCell(iterableGPR));
    addSlowCase(branchIfNotType(iterableGPR, PyTupleType));
    addSlowCase(branch32(NotEqual, Address(iterableGPR, PyTuple::offsetOfLength()), TrustedImm32(bytecode.m_argc)));
    for (unsigned i = 0; i < bytecode.m_argc; ++i) {
        load64(Address(iterableGPR, PyTuple::offsetOfValues() + i * sizeof(EncodedJSValue)), scratchGPR);
        emitPutVirtualRegister(bytecode.m_argv - static_cast<int>(i), scratchGPR);
    }
}

// ---- Attributes

// They are remembered as properties are: see AccessType::PyGetAttr and its like, and tryCachePyGetAttr().

void JIT::emit_op_py_get_attr(const JSInstruction* currentInstruction)
{
    auto bytecode = currentInstruction->as<OpPyGetAttr>();
    const Identifier* ident = &(m_unlinkedCodeBlock->identifier(bytecode.m_property));

    using BaselineJITRegisters::GetById::baseGPR;
    using BaselineJITRegisters::GetById::resultGPR;
    using BaselineJITRegisters::GetById::propertyCacheGPR;

    emitGetVirtualRegister(bytecode.m_base, baseGPR);

    auto [ propertyCache, propertyCacheIndex ] = addUnlinkedPropertyInlineCache();
    loadPropertyInlineCache(propertyCacheIndex, propertyCacheGPR);

    emitJumpSlowCaseIfNotJSCell(baseGPR, bytecode.m_base);

    JITGetByIdGenerator gen(
        nullptr, propertyCache, JITType::BaselineJIT, CodeOrigin(m_bytecodeIndex), CallSiteIndex(m_bytecodeIndex), RegisterSet::stubUnavailableRegisters(),
        CacheableIdentifier::createFromIdentifierOwnedByCodeBlock(m_unlinkedCodeBlock, *ident), baseGPR, resultGPR, propertyCacheGPR, AccessType::PyGetAttr, CacheType::GetByIdSelf);

    gen.generateDataICFastPath(*this);
    resetSP(); // We might OSR exit here, so we need to conservatively reset SP
    addSlowCase();
    m_getByIds.append(gen);

    setFastPathResumePoint();
    emitValueProfilingSite(bytecode, resultGPR);
    emitPutVirtualRegister(bytecode.m_dst, resultGPR);
}

void JIT::emitSlow_op_py_get_attr(const JSInstruction* currentInstruction, Vector<SlowCaseEntry>::iterator& iter)
{
    emitSlow_op_get_by_id(currentInstruction, iter);
}

void JIT::emit_op_py_load_method(const JSInstruction* currentInstruction)
{
    auto bytecode = currentInstruction->as<OpPyLoadMethod>();
    const Identifier* ident = &(m_unlinkedCodeBlock->identifier(bytecode.m_property));

    using BaselineJITRegisters::PyLoadMethod::baseGPR;
    using BaselineJITRegisters::PyLoadMethod::resultGPR;
    using BaselineJITRegisters::PyLoadMethod::selfGPR;
    using BaselineJITRegisters::PyLoadMethod::propertyCacheGPR;

    emitGetVirtualRegister(bytecode.m_base, baseGPR);

    auto [ propertyCache, propertyCacheIndex ] = addUnlinkedPropertyInlineCache();
    loadPropertyInlineCache(propertyCacheIndex, propertyCacheGPR);

    emitJumpSlowCaseIfNotJSCell(baseGPR, bytecode.m_base);
    move(baseGPR, selfGPR);

    JITGetByIdGenerator gen(
        nullptr, propertyCache, JITType::BaselineJIT, CodeOrigin(m_bytecodeIndex), CallSiteIndex(m_bytecodeIndex), RegisterSet::stubUnavailableRegisters(),
        CacheableIdentifier::createFromIdentifierOwnedByCodeBlock(m_unlinkedCodeBlock, *ident), baseGPR, resultGPR, propertyCacheGPR, AccessType::PyLoadMethod, CacheType::GetByIdPrototype);

    gen.generateDataICFastPath(*this);
    resetSP(); // We might OSR exit here, so we need to conservatively reset SP
    addSlowCase();
    m_getByIds.append(gen);

    setFastPathResumePoint();
    emitValueProfilingSite(bytecode, resultGPR);
    emitPutVirtualRegister(bytecode.m_dst, resultGPR);
    emitPutVirtualRegister(bytecode.m_self, selfGPR);
}

void JIT::emitSlow_op_py_load_method(const JSInstruction* currentInstruction, Vector<SlowCaseEntry>::iterator& iter)
{
    emitSlow_op_get_by_id(currentInstruction, iter);
}

void JIT::emit_op_py_set_attr(const JSInstruction* currentInstruction)
{
    auto bytecode = currentInstruction->as<OpPySetAttr>();
    const Identifier* ident = &(m_unlinkedCodeBlock->identifier(bytecode.m_property));

    using BaselineJITRegisters::PutById::baseGPR;
    using BaselineJITRegisters::PutById::valueGPR;
    using BaselineJITRegisters::PutById::propertyCacheGPR;
    using BaselineJITRegisters::PutById::scratch1GPR;

    emitGetVirtualRegister(bytecode.m_base, baseGPR);
    emitGetVirtualRegister(bytecode.m_value, valueGPR);

    auto [ propertyCache, propertyCacheIndex ] = addUnlinkedPropertyInlineCache();
    loadPropertyInlineCache(propertyCacheIndex, propertyCacheGPR);

    emitJumpSlowCaseIfNotJSCell(baseGPR, bytecode.m_base);

    JITPutByIdGenerator gen(
        nullptr, propertyCache, JITType::BaselineJIT, CodeOrigin(m_bytecodeIndex), CallSiteIndex(m_bytecodeIndex), RegisterSet::stubUnavailableRegisters(),
        CacheableIdentifier::createFromIdentifierOwnedByCodeBlock(m_unlinkedCodeBlock, *ident), baseGPR, valueGPR, propertyCacheGPR, scratch1GPR, AccessType::PySetAttr);

    gen.generateDataICFastPath(*this);
    resetSP(); // We might OSR exit here, so we need to conservatively reset SP
    addSlowCase();
    m_putByIds.append(gen);

    // IC can write new Structure without write-barrier if a base is cell.
    emitWriteBarrier(bytecode.m_base, ShouldFilterBase);
}

void JIT::emitSlow_op_py_set_attr(const JSInstruction* currentInstruction, Vector<SlowCaseEntry>::iterator& iter)
{
    emitSlow_op_put_by_id(currentInstruction, iter);
}

// ---- Names, and where a frame begins and ends

void JIT::emit_op_py_load_global(const JSInstruction* currentInstruction)
{
    auto bytecode = currentInstruction->as<OpPyLoadGlobal>();
    emitGetVirtualRegister(bytecode.m_globals, regT0);
    load32FromMetadata(bytecode, OpPyLoadGlobal::Metadata::offsetOfGlobalsStructureID(), regT1);
    addSlowCase(branch32(NotEqual, Address(regT0, JSCell::structureIDOffset()), regT1));
    load32FromMetadata(bytecode, OpPyLoadGlobal::Metadata::offsetOfBuiltinsStructureID(), regT1);
    Jump isInGlobals = branchTest32(Zero, regT1);
    emitGetVirtualRegister(bytecode.m_builtins, regT0);
    addSlowCase(branch32(NotEqual, Address(regT0, JSCell::structureIDOffset()), regT1));
    isInGlobals.link(this);
    load32FromMetadata(bytecode, OpPyLoadGlobal::Metadata::offsetOfOffset(), regT1);
    loadProperty(regT0, regT1, regT2);
    emitValueProfilingSite(bytecode, regT2);
    emitPutVirtualRegister(bytecode.m_dst, regT2);
}

void JIT::emit_op_py_enter(const JSInstruction*)
{
    load32(vm().addressOfPythonDepth(), regT0);
    add32(TrustedImm32(1), regT0);
    store32(regT0, vm().addressOfPythonDepth());
    load32(vm().addressOfPythonLimitUnlessWatched(), regT1);
    addSlowCase(branch32(Above, regT0, regT1));
}

void JIT::emit_op_py_line(const JSInstruction*)
{
    addSlowCase(branchTest32(Zero, AbsoluteAddress(vm().addressOfPythonLimitUnlessWatched())));
}

void JIT::emitSlow_op_py_line(const JSInstruction*, Vector<SlowCaseEntry>::iterator& iter)
{
    emitSlowCaseCall(iter, slow_path_py_line);
    // frame.f_lineno = n. Where it goes to is just after another of these, where nothing is expected to be in any register.
    Jump goesOn = branchTestPtr(Zero, returnValueGPR2);
    farJump(returnValueGPR2, JSEntryPtrTag);
    goesOn.link(this);
}

void JIT::emit_op_py_call(const JSInstruction* instruction)
{
    emit_op_py_line(instruction);
}

void JIT::emit_op_py_branch(const JSInstruction* instruction)
{
    emit_op_py_line(instruction);
}

void JIT::emit_op_py_jump(const JSInstruction* instruction)
{
    emit_op_py_line(instruction);
}

void JIT::emit_op_py_leave(const JSInstruction*)
{
    Jump isNotWatched = branchTest32(NonZero, AbsoluteAddress(vm().addressOfPythonLimitUnlessWatched()));
    JITSlowPathCall slowPathCall(this, slow_path_py_leave);
    slowPathCall.call();
    isNotWatched.link(this);
    sub32(TrustedImm32(1), AbsoluteAddress(vm().addressOfPythonDepth()));
}

void JIT::emit_op_py_ret(const JSInstruction* currentInstruction)
{
    auto bytecode = currentInstruction->as<OpPyRet>();
    Jump isWatched = branchTest32(Zero, AbsoluteAddress(vm().addressOfPythonLimitUnlessWatched()));
    emitGetVirtualRegister(bytecode.m_frame, regT0);
    // This is not a slow case, since those end by going on to the next instruction, and there may be none.
    Jump hasNoFrameObject = branch64(Equal, regT0, TrustedImm64(JSValue::encode(jsUndefined())));
    isWatched.link(this);
    JITSlowPathCall slowPathCall(this, slow_path_py_leave_frame);
    slowPathCall.call();
    hasNoFrameObject.link(this);
    sub32(TrustedImm32(1), AbsoluteAddress(vm().addressOfPythonDepth()));
    emitGetVirtualRegister(bytecode.m_value, returnValueGPR);
    jumpThunk(CodeLocationLabel { vm().getCTIStub(CommonJITThunkID::ReturnFromBaseline).retaggedCode<NoPtrTag>() });
}

} // namespace JSC

#endif // ENABLE(JIT)
