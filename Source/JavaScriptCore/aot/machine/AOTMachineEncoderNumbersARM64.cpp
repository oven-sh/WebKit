/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineEncoder.h"

#if ENABLE(AOT) && CPU(ARM64)

#include "AllowMacroScratchRegisterUsage.h"

namespace JSC { namespace AOT { namespace Machine {

struct ARM64NumberOperands {
    FPRReg left;
    FPRReg right;
};

static Reg takeARM64NumberSource(Encoder& encoder, const Operation* user, unsigned childIndex, Type type, LeafCursor& leaves)
{
    const Operation* child = user->child(childIndex);
    RELEASE_ASSERT(child->type() == type && (!encoder.m_liveness.isFused(child) || child->opcode() == Trunc));
    Bank bank = bankOf(type);

    Location source = leaves.take();
    switch (source.kind()) {
    case Location::Register:
        RELEASE_ASSERT(bankOf(source.reg()) == bank);
        return source.reg();
    case Location::Immediate: {
        Reg scratch = bank == Bank::FP ? Reg(scratchFPR) : Reg(scratchGPR);
        encoder.encodeMaterialize(child, scratch);
        return scratch;
    }
    case Location::None:
    case Location::Spill:
    case Location::SlotBytes:
    case Location::Outgoing:
    case Location::InstanceWord:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static FPRReg takeARM64NumberOperand(Encoder& encoder, const Step& step, Type type)
{
    ASSERT(type.isFloat());
    LeafCursor leaves(step);
    return takeARM64NumberSource(encoder, step.operation, 0, type, leaves).fpr();
}

static ARM64NumberOperands takeARM64NumberOperands(Encoder& encoder, const Step& step)
{
    RELEASE_ASSERT(step.sources[0].isRegister() || step.sources[1].isRegister());

    const Operation* operation = step.operation;
    Type type = operation->type();
    ASSERT(type.isFloat());
    LeafCursor leaves(step);
    ARM64NumberOperands operands;
    operands.left = takeARM64NumberSource(encoder, operation, 0, type, leaves).fpr();
    operands.right = takeARM64NumberSource(encoder, operation, 1, type, leaves).fpr();
    return operands;
}

static void encodeARM64BitwiseCast(Encoder& encoder, const Step& step)
{
    const Operation* operation = step.operation;
    Type typeOfSource = operation->child(0)->type();
    RELEASE_ASSERT(typeOfSource.isNumeric() && widthOf(typeOfSource) == widthOf(operation->type()));
    bool isWide = widthOf(typeOfSource) == Width64;

    CCallHelpers& jit = encoder.m_jit;
    LeafCursor leaves(step);
    Reg source = takeARM64NumberSource(encoder, operation, 0, typeOfSource, leaves);
    Reg destination = step.destination.reg();

    if (source.isGPR() && destination.isFPR()) {
        if (isWide)
            jit.move64ToDouble(source.gpr(), destination.fpr());
        else
            jit.move32ToFloat(source.gpr(), destination.fpr());
        return;
    }
    if (source.isFPR() && destination.isGPR()) {
        if (isWide)
            jit.moveDoubleTo64(source.fpr(), destination.gpr());
        else
            jit.moveFloatTo32(source.fpr(), destination.gpr());
        return;
    }
    if (source.isFPR()) {
        jit.moveDouble(source.fpr(), destination.fpr());
        return;
    }
    jit.move(source.gpr(), destination.gpr());
}

void Encoder::encodeNumber(const Step& step)
{
    ASSERT(step.kind == StepKind::Execute);
    const Operation* operation = step.operation;
    Opcode opcode = operation->opcode();
    Type type = operation->type();
    RELEASE_ASSERT(type.isFloat() || (opcode == BitwiseCast && type.isInt()));
    RELEASE_ASSERT(step.destination.isRegister() && bankOf(step.destination.reg()) == bankOf(type));

    AllowMacroScratchRegisterUsage allowScratch(m_jit);
    bool isDouble = type == Double;
    FPRReg destination = type.isFloat() ? step.destination.fpr() : InvalidFPRReg;

    switch (opcode) {
    case Add: {
        ARM64NumberOperands operands = takeARM64NumberOperands(*this, step);
        if (isDouble)
            m_jit.addDouble(operands.left, operands.right, destination);
        else
            m_jit.addFloat(operands.left, operands.right, destination);
        return;
    }

    case Sub: {
        ARM64NumberOperands operands = takeARM64NumberOperands(*this, step);
        if (isDouble)
            m_jit.subDouble(operands.left, operands.right, destination);
        else
            m_jit.subFloat(operands.left, operands.right, destination);
        return;
    }

    case Mul: {
        ARM64NumberOperands operands = takeARM64NumberOperands(*this, step);
        if (isDouble)
            m_jit.mulDouble(operands.left, operands.right, destination);
        else
            m_jit.mulFloat(operands.left, operands.right, destination);
        return;
    }

    case Div: {
        ARM64NumberOperands operands = takeARM64NumberOperands(*this, step);
        if (isDouble)
            m_jit.divDouble(operands.left, operands.right, destination);
        else
            m_jit.divFloat(operands.left, operands.right, destination);
        return;
    }

    case FMin: {
        ARM64NumberOperands operands = takeARM64NumberOperands(*this, step);
        if (isDouble)
            m_jit.doubleMin(operands.left, operands.right, destination);
        else
            m_jit.floatMin(operands.left, operands.right, destination);
        return;
    }

    case FMax: {
        ARM64NumberOperands operands = takeARM64NumberOperands(*this, step);
        if (isDouble)
            m_jit.doubleMax(operands.left, operands.right, destination);
        else
            m_jit.floatMax(operands.left, operands.right, destination);
        return;
    }

    case Neg: {
        FPRReg operand = takeARM64NumberOperand(*this, step, type);
        if (isDouble)
            m_jit.negateDouble(operand, destination);
        else
            m_jit.negateFloat(operand, destination);
        return;
    }

    case Abs: {
        FPRReg operand = takeARM64NumberOperand(*this, step, type);
        if (isDouble)
            m_jit.absDouble(operand, destination);
        else
            m_jit.absFloat(operand, destination);
        return;
    }

    case Ceil: {
        FPRReg operand = takeARM64NumberOperand(*this, step, type);
        if (isDouble)
            m_jit.ceilDouble(operand, destination);
        else
            m_jit.ceilFloat(operand, destination);
        return;
    }

    case Floor: {
        FPRReg operand = takeARM64NumberOperand(*this, step, type);
        if (isDouble)
            m_jit.floorDouble(operand, destination);
        else
            m_jit.floorFloat(operand, destination);
        return;
    }

    case FTrunc: {
        FPRReg operand = takeARM64NumberOperand(*this, step, type);
        if (isDouble)
            m_jit.roundTowardZeroDouble(operand, destination);
        else
            m_jit.roundTowardZeroFloat(operand, destination);
        return;
    }

    case Sqrt: {
        FPRReg operand = takeARM64NumberOperand(*this, step, type);
        if (isDouble)
            m_jit.sqrtDouble(operand, destination);
        else
            m_jit.sqrtFloat(operand, destination);
        return;
    }

    case IToD: {
        Type typeOfInteger = operation->child(0)->type();
        RELEASE_ASSERT(isDouble && typeOfInteger.isInt());
        LeafCursor leaves(step);
        GPRReg integer = takeARM64NumberSource(*this, operation, 0, typeOfInteger, leaves).gpr();
        if (typeOfInteger == Int64)
            m_jit.convertInt64ToDouble(integer, destination);
        else
            m_jit.convertInt32ToDouble(integer, destination);
        return;
    }

    case FloatToDouble:
        RELEASE_ASSERT(isDouble);
        m_jit.convertFloatToDouble(takeARM64NumberOperand(*this, step, Float), destination);
        return;

    case DoubleToFloat:
        RELEASE_ASSERT(!isDouble);
        m_jit.convertDoubleToFloat(takeARM64NumberOperand(*this, step, Double), destination);
        return;

    case BitwiseCast:
        encodeARM64BitwiseCast(*this, step);
        return;

    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT) && CPU(ARM64)
