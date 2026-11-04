/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineEncoder.h"

#if ENABLE(AOT) && CPU(ARM64)

#include "AllowMacroScratchRegisterUsage.h"

namespace JSC { namespace AOT { namespace Machine {

// Where `constant` is set it is the second source, and `right` names no register.
struct IntegerSources {
    GPRReg left { InvalidGPRReg };
    GPRReg right { InvalidGPRReg };
    const Operation* constant { nullptr };
};

static MacroAssembler::TrustedImm32 integerConstant32Of(const Operation* constant)
{
    return MacroAssembler::TrustedImm32(static_cast<int32_t>(constant->asInt()));
}

static MacroAssembler::TrustedImm64 integerConstant64Of(const Operation* constant)
{
    return MacroAssembler::TrustedImm64(constant->asInt());
}

// An operation without position that the instruction has no field for is made in x16, so at most one source of an operation comes this way.
static GPRReg registerOfIntegerSource(Encoder& encoder, Location source, const Operation* child)
{
    if (source.isRegister())
        return source.gpr();

    RELEASE_ASSERT(child->hasNoPosition());
    GPRReg scratch = encoder.m_jit.scratchRegister();
    encoder.encodeMaterialize(child, Reg(scratch));
    return scratch;
}

static GPRReg integerSourceInRegister(Encoder& encoder, const Step& step)
{
    LeafCursor leaves(step);
    return registerOfIntegerSource(encoder, leaves.take(), step.operation->child(0));
}

static IntegerSources integerSourcesInRegisters(Encoder& encoder, const Step& step)
{
    const Operation* operation = step.operation;
    LeafCursor leaves(step);
    Location left = leaves.take();
    Location right = leaves.take();
    RELEASE_ASSERT(left.isRegister() || right.isRegister());

    IntegerSources sources;
    sources.left = registerOfIntegerSource(encoder, left, operation->child(0));
    sources.right = registerOfIntegerSource(encoder, right, operation->child(1));
    return sources;
}

static IntegerSources integerSourcesOrConstant(Encoder& encoder, const Step& step)
{
    const Operation* second = step.operation->child(1);
    LeafCursor leaves(step);
    Location left = leaves.take();
    Location right = leaves.take();
    if (!right.isImmediate() || !second->hasInt())
        return integerSourcesInRegisters(encoder, step);

    // MacroAssembler makes a constant that fits no field of the instruction in x16.
    RELEASE_ASSERT(left.isRegister());
    IntegerSources sources;
    sources.left = left.gpr();
    sources.constant = second;
    return sources;
}

static void encodeARM64ScaledSum(Encoder& encoder, const Step& step)
{
    LeafCursor leaves(step);
    AddressForm form = encoder.resolveScaledSum(step.operation, leaves);
    GPRReg destination = step.destination.gpr();
    // In an addition of a shifted register the number of the stack pointer names the zero register.
    RELEASE_ASSERT(!form.offset && form.base != MacroAssembler::stackPointerRegister && form.index != MacroAssembler::stackPointerRegister && destination != MacroAssembler::stackPointerRegister);

    switch (form.extension) {
    case IndexExtension::None:
        encoder.m_jit.addLeftShift64(form.base, form.index, MacroAssembler::TrustedImm32(form.shift), destination);
        return;
    case IndexExtension::ZeroExtend32:
        encoder.m_jit.m_assembler.add<64>(destination, form.base, form.index, ARM64Assembler::UXTW, form.shift);
        return;
    case IndexExtension::SignExtend32:
        encoder.m_jit.m_assembler.add<64>(destination, form.base, form.index, ARM64Assembler::SXTW, form.shift);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool Encoder::tryEncodeSumAndConstant(const Step&, const Step&)
{
    return false;
}

void Encoder::encodeSumThatSetsFlags(const Step& step)
{
    SumTestedByOverflow test = sumTestedByOverflowOf(m_liveness, step.operation);
    RELEASE_ASSERT(step.destination.isRegister() && step.sources[0].isRegister());
    GPRReg destination = step.destination.gpr();
    GPRReg left = step.sources[0].gpr();
    bool subtracts = test.subtracts();

    if (step.sources[1].isImmediate()) {
        // acceptsImmediate() has allowed it: twelve bits, shifted by twelve or not. One below zero goes the other way, which overflows for the same operands.
        int64_t constant = test.right()->asInt();
        uint64_t magnitude = constant < 0 ? uint64_t { 0 } - static_cast<uint64_t>(constant) : static_cast<uint64_t>(constant);
        if (constant < 0)
            subtracts = !subtracts;
        int shift = magnitude & ~uint64_t { 0xfff } ? 12 : 0;
        RELEASE_ASSERT(!(magnitude & ~(uint64_t { 0xfff } << shift)));
        UInt12 immediate(static_cast<int>(magnitude >> shift));
        if (subtracts)
            m_jit.m_assembler.sub<32, ARM64Assembler::S>(destination, left, immediate, shift);
        else
            m_jit.m_assembler.add<32, ARM64Assembler::S>(destination, left, immediate, shift);
        return;
    }

    RELEASE_ASSERT(step.sources[1].isRegister());
    GPRReg right = step.sources[1].gpr();
    if (subtracts)
        m_jit.m_assembler.sub<32, ARM64Assembler::S>(destination, left, right);
    else
        m_jit.m_assembler.add<32, ARM64Assembler::S>(destination, left, right);
}

// As MacroAssemblerARM64::makeBranch() writes it.
MacroAssembler::Jump Encoder::jumpOnOverflow(bool whenItOverflowed)
{
    ARM64Assembler::Condition condition = whenItOverflowed ? ARM64Assembler::ConditionVS : ARM64Assembler::ConditionVC;
    m_jit.m_assembler.b_cond(condition);
    AssemblerLabel label = m_jit.m_assembler.labelIgnoringWatchpoints();
    m_jit.m_assembler.nop();
    return MacroAssembler::Jump(label, ARM64Assembler::JumpCondition, condition);
}

void Encoder::encodeInteger(const Step& step)
{
    ASSERT(step.kind == StepKind::Execute);
    const Operation* operation = step.operation;
    ASSERT(operation->type().isInt());
#if ASSERT_ENABLED
    for (const Operation* child : operation->children())
        ASSERT(!m_liveness.isFused(child) || child->opcode() == Trunc || addsScaledIndex(m_liveness, operation));
#endif

    RELEASE_ASSERT(step.destination.isRegister());
    GPRReg destination = step.destination.gpr();
    bool is32 = widthOf(operation->type()) == Width32;

    AllowMacroScratchRegisterUsage allowScratch(m_jit);

    switch (operation->opcode()) {
    case Opaque: {
        Location source = LeafCursor(step).take();
        if (source.isRegister()) {
            m_jit.move(source.gpr(), destination);
            return;
        }
        RELEASE_ASSERT(operation->child(0)->hasNoPosition());
        encodeMaterialize(operation->child(0), step.destination.reg());
        return;
    }

    case Add: {
        if (m_liveness.goesByOverflow(operation)) {
            encodeSumThatSetsFlags(step);
            return;
        }
        if (addsScaledIndex(m_liveness, operation)) {
            encodeARM64ScaledSum(*this, step);
            return;
        }
        IntegerSources sources = integerSourcesOrConstant(*this, step);
        if (sources.constant && is32)
            m_jit.add32(integerConstant32Of(sources.constant), sources.left, destination);
        else if (sources.constant)
            m_jit.add64(integerConstant64Of(sources.constant), sources.left, destination);
        else if (is32)
            m_jit.add32(sources.left, sources.right, destination);
        else
            m_jit.add64(sources.left, sources.right, destination);
        return;
    }

    case Sub: {
        if (m_liveness.goesByOverflow(operation)) {
            encodeSumThatSetsFlags(step);
            return;
        }
        IntegerSources sources = integerSourcesOrConstant(*this, step);
        if (sources.constant && is32)
            m_jit.sub32(sources.left, integerConstant32Of(sources.constant), destination);
        else if (sources.constant)
            m_jit.sub64(sources.left, integerConstant64Of(sources.constant), destination);
        else if (is32)
            m_jit.sub32(sources.left, sources.right, destination);
        else
            m_jit.sub64(sources.left, sources.right, destination);
        return;
    }

    case Mul: {
        IntegerSources sources = integerSourcesInRegisters(*this, step);
        if (is32)
            m_jit.mul32(sources.left, sources.right, destination);
        else
            m_jit.mul64(sources.left, sources.right, destination);
        return;
    }

    case MulHigh: {
        IntegerSources sources = integerSourcesInRegisters(*this, step);
        if (is32)
            m_jit.mulHigh32(sources.left, sources.right, destination);
        else
            m_jit.mulHigh64(sources.left, sources.right, destination);
        return;
    }

    case UMulHigh: {
        IntegerSources sources = integerSourcesInRegisters(*this, step);
        if (is32)
            m_jit.uMulHigh32(sources.left, sources.right, destination);
        else
            m_jit.uMulHigh64(sources.left, sources.right, destination);
        return;
    }

    // The instruction gives 0 for a division by 0 and the least integer for the least integer by -1, which is what a chill Div asks.
    case Div: {
        IntegerSources sources = integerSourcesInRegisters(*this, step);
        if (is32)
            m_jit.div32(sources.left, sources.right, destination);
        else
            m_jit.div64(sources.left, sources.right, destination);
        return;
    }

    case UDiv: {
        IntegerSources sources = integerSourcesInRegisters(*this, step);
        if (is32)
            m_jit.uDiv32(sources.left, sources.right, destination);
        else
            m_jit.uDiv64(sources.left, sources.right, destination);
        return;
    }

    case Neg: {
        GPRReg source = integerSourceInRegister(*this, step);
        if (is32)
            m_jit.neg32(source, destination);
        else
            m_jit.neg64(source, destination);
        return;
    }

    case BitAnd: {
        IntegerSources sources = integerSourcesOrConstant(*this, step);
        if (sources.constant && is32)
            m_jit.and32(integerConstant32Of(sources.constant), sources.left, destination);
        else if (sources.constant)
            m_jit.and64(integerConstant64Of(sources.constant), sources.left, destination);
        else if (is32)
            m_jit.and32(sources.left, sources.right, destination);
        else
            m_jit.and64(sources.left, sources.right, destination);
        return;
    }

    case BitOr: {
        IntegerSources sources = integerSourcesOrConstant(*this, step);
        if (sources.constant && is32)
            m_jit.or32(integerConstant32Of(sources.constant), sources.left, destination);
        else if (sources.constant)
            m_jit.or64(integerConstant64Of(sources.constant), sources.left, destination);
        else if (is32)
            m_jit.or32(sources.left, sources.right, destination);
        else
            m_jit.or64(sources.left, sources.right, destination);
        return;
    }

    case BitXor: {
        IntegerSources sources = integerSourcesOrConstant(*this, step);
        if (sources.constant && is32)
            m_jit.xor32(integerConstant32Of(sources.constant), sources.left, destination);
        else if (sources.constant)
            m_jit.xor64(integerConstant64Of(sources.constant), sources.left, destination);
        else if (is32)
            m_jit.xor32(sources.left, sources.right, destination);
        else
            m_jit.xor64(sources.left, sources.right, destination);
        return;
    }

    // The amount is an Int32 whatever the width. The instruction takes it modulo the width, and so does MacroAssembler with a constant.
    case Shl: {
        IntegerSources sources = integerSourcesOrConstant(*this, step);
        if (sources.constant && is32)
            m_jit.lshift32(sources.left, integerConstant32Of(sources.constant), destination);
        else if (sources.constant)
            m_jit.lshift64(sources.left, integerConstant32Of(sources.constant), destination);
        else if (is32)
            m_jit.lshift32(sources.left, sources.right, destination);
        else
            m_jit.lshift64(sources.left, sources.right, destination);
        return;
    }

    case SShr: {
        IntegerSources sources = integerSourcesOrConstant(*this, step);
        if (sources.constant && is32)
            m_jit.rshift32(sources.left, integerConstant32Of(sources.constant), destination);
        else if (sources.constant)
            m_jit.rshift64(sources.left, integerConstant32Of(sources.constant), destination);
        else if (is32)
            m_jit.rshift32(sources.left, sources.right, destination);
        else
            m_jit.rshift64(sources.left, sources.right, destination);
        return;
    }

    case ZShr: {
        IntegerSources sources = integerSourcesOrConstant(*this, step);
        if (sources.constant && is32)
            m_jit.urshift32(sources.left, integerConstant32Of(sources.constant), destination);
        else if (sources.constant)
            m_jit.urshift64(sources.left, integerConstant32Of(sources.constant), destination);
        else if (is32)
            m_jit.urshift32(sources.left, sources.right, destination);
        else
            m_jit.urshift64(sources.left, sources.right, destination);
        return;
    }

    case Clz: {
        GPRReg source = integerSourceInRegister(*this, step);
        if (is32)
            m_jit.countLeadingZeros32(source, destination);
        else
            m_jit.countLeadingZeros64(source, destination);
        return;
    }

    case PopCount: {
        GPRReg source = integerSourceInRegister(*this, step);
        if (is32)
            m_jit.countPopulation32(source, destination, scratchFPR);
        else
            m_jit.countPopulation64(source, destination, scratchFPR);
        return;
    }

    case SExt8: {
        GPRReg source = integerSourceInRegister(*this, step);
        if (is32)
            m_jit.signExtend8To32(source, destination);
        else
            m_jit.signExtend8To64(source, destination);
        return;
    }

    case SExt16: {
        GPRReg source = integerSourceInRegister(*this, step);
        if (is32)
            m_jit.signExtend16To32(source, destination);
        else
            m_jit.signExtend16To64(source, destination);
        return;
    }

    // The upper half of a register that holds an Int32 is unspecified, so an extension is an instruction where the registers agree too, unless isFreeInRegisterOfChild() says otherwise.
    case SExt32: {
        ASSERT(operation->type() == Int64 && operation->child(0)->type() == Int32);
        GPRReg source = integerSourceInRegister(*this, step);
        if (source != destination || !isFreeInRegisterOfChild(m_liveness, operation))
            m_jit.signExtend32To64(source, destination);
        return;
    }

    case ZExt32: {
        ASSERT(operation->type() == Int64 && operation->child(0)->type() == Int32);
        GPRReg source = integerSourceInRegister(*this, step);
        if (source != destination || !isFreeInRegisterOfChild(m_liveness, operation))
            m_jit.zeroExtend32ToWord(source, destination);
        return;
    }

    case Trunc:
        ASSERT(operation->type() == Int32 && operation->child(0)->type() == Int64);
        m_jit.move(integerSourceInRegister(*this, step), destination);
        return;

    case Nop:
    case Const32:
    case Const64:
    case ConstFloat:
    case ConstDouble:
    case FramePointer:
    case SlotBase:
    case AddressOf:
    case NumberOf:
    case InstanceAddressOf:
    case ArgumentReg:
    case Abs:
    case Ceil:
    case Floor:
    case FTrunc:
    case Sqrt:
    case FMin:
    case FMax:
    case BitwiseCast:
    case IToD:
    case FloatToDouble:
    case DoubleToFloat:
    case Equal:
    case NotEqual:
    case LessThan:
    case GreaterThan:
    case LessEqual:
    case GreaterEqual:
    case Above:
    case Below:
    case AboveEqual:
    case BelowEqual:
    case EqualOrUnordered:
    case Select:
    case Load8Z:
    case Load8S:
    case Load16Z:
    case Load16S:
    case Load:
    case Store8:
    case Store16:
    case Store:
    case Fence:
    case Phi:
    case Upsilon:
    case Extract:
    case Patch:
    case Check:
    case Jump:
    case Branch:
    case Switch:
    case Return:
    case Oops:
    // The builder makes Sub(a, Mul(Div(a, b), b)) of both.
    case Mod:
    case UMod:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT) && CPU(ARM64)
