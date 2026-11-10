/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineEncoder.h"

#if ENABLE(AOT) && CPU(X86_64)

#include "AllowMacroScratchRegisterUsage.h"
#include <bit>
#include <type_traits>
#include <utility>

namespace JSC { namespace AOT { namespace Machine {

enum class X86IntegerInstruction : uint8_t {
    Add,
    Subtract,
    Multiply,
    And,
    Or,
    ExclusiveOr,
};

enum class X86IntegerShift : uint8_t {
    Left,
    ArithmeticRight,
    LogicalRight,
    RotateRight,
};

enum class X86IntegerExtension : uint8_t {
    SignExtend8,
    SignExtend16,
    SignExtend32,
    ZeroExtend32,
};

struct X86IntegerOperand {
    enum Kind : uint8_t {
        InRegister,
        AtAddress,
        AtIndexedAddress,
        IsConstant,
    };

    static X86IntegerOperand inRegister(GPRReg gpr)
    {
        X86IntegerOperand result;
        result.kind = InRegister;
        result.gpr = gpr;
        return result;
    }

    static X86IntegerOperand at(MacroAssembler::Address address)
    {
        X86IntegerOperand result;
        result.kind = AtAddress;
        result.gpr = address.base;
        result.offset = address.offset;
        return result;
    }

    static X86IntegerOperand at(MacroAssembler::BaseIndex address)
    {
        X86IntegerOperand result;
        result.kind = AtIndexedAddress;
        result.gpr = address.base;
        result.index = address.index;
        result.scale = address.scale;
        result.offset = address.offset;
        return result;
    }

    static X86IntegerOperand forConstant(int64_t value)
    {
        X86IntegerOperand result;
        result.kind = IsConstant;
        result.constant = value;
        return result;
    }

    bool isInRegister() const { return kind == InRegister; }
    bool isMemory() const { return kind == AtAddress || kind == AtIndexedAddress; }
    bool isConstant() const { return kind == IsConstant; }

    bool mentions(GPRReg reg) const
    {
        switch (kind) {
        case InRegister:
        case AtAddress:
            return gpr == reg;
        case AtIndexedAddress:
            return gpr == reg || index == reg;
        case IsConstant:
            return false;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    Kind kind { IsConstant };
    GPRReg gpr { InvalidGPRReg };
    GPRReg index { InvalidGPRReg };
    MacroAssembler::Scale scale { MacroAssembler::TimesOne };
    int32_t offset { 0 };
    int64_t constant { 0 };
};

template<typename Functor>
static void withX86IntegerAddress(const X86IntegerOperand& operand, const Functor& functor)
{
    if (operand.kind == X86IntegerOperand::AtAddress) {
        functor(MacroAssembler::Address(operand.gpr, operand.offset));
        return;
    }
    RELEASE_ASSERT(operand.kind == X86IntegerOperand::AtIndexedAddress);
    functor(MacroAssembler::BaseIndex(operand.gpr, operand.index, operand.scale, operand.offset));
}

static NEVER_INLINE X86IntegerOperand takeX86IntegerOperandFromFusedLoad(Encoder& encoder, const Operation* load, LeafCursor& cursor)
{
    RELEASE_ASSERT(isLoad(load->opcode()) && !load->traps() && !encoder.m_liveness.of(load).has(ValueFlag::TakesOverTrap));
    CCallHelpers& jit = encoder.m_jit;
    AllowMacroScratchRegisterUsage allowScratch(jit);

    AddressForm form = encoder.resolveAddress(load, cursor);
    X86IntegerOperand memory;
    switch (form.kind) {
    case AddressForm::BaseOffset:
        memory = X86IntegerOperand::at(form.address());
        break;
    case AddressForm::BaseIndex:
        RELEASE_ASSERT(form.extension == IndexExtension::None && form.shift <= 3);
        memory = X86IntegerOperand::at(MacroAssembler::BaseIndex(form.base, form.index, static_cast<MacroAssembler::Scale>(form.shift), form.offset));
        break;
    case AddressForm::InstanceSymbol:
        encoder.m_relocations.moveInstanceAddress(form.symbol, instanceGPR, scratchGPR);
        memory = X86IntegerOperand::at(MacroAssembler::Address(scratchGPR, form.offset));
        break;
    }

    Opcode opcode = load->opcode();
    if (opcode == Load)
        return memory;

    withX86IntegerAddress(memory, [&](auto address) {
        if (opcode == Load8Z)
            jit.load8(address, scratchGPR);
        else if (opcode == Load8S)
            jit.load8SignedExtendTo32(address, scratchGPR);
        else if (opcode == Load16Z)
            jit.load16(address, scratchGPR);
        else {
            RELEASE_ASSERT(opcode == Load16S);
            jit.load16SignedExtendTo32(address, scratchGPR);
        }
    });
    return X86IntegerOperand::inRegister(scratchGPR);
}

static ALWAYS_INLINE X86IntegerOperand takeX86IntegerOperand(Encoder& encoder, const Operation* user, unsigned childIndex, LeafCursor& cursor)
{
    const Operation* child = inlineChildWithoutCheck(user, childIndex);
    if (encoder.isFused(child) && child->opcode() != Trunc)
        return takeX86IntegerOperandFromFusedLoad(encoder, child, cursor);

    Location location = cursor.take();
    switch (location.kind()) {
    case Location::Register:
        return X86IntegerOperand::inRegister(location.gpr());
    case Location::Spill:
    case Location::Outgoing:
    case Location::InstanceWord:
        return X86IntegerOperand::at(encoder.addressOf(location));
    case Location::Immediate:
        RELEASE_ASSERT(child->hasInt());
        return X86IntegerOperand::forConstant(child->asInt());
    case Location::None:
    case Location::SlotBytes:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ALWAYS_INLINE X86IntegerOperand takeFirstX86IntegerOperand(Encoder& encoder, const Step& step)
{
    LeafCursor cursor(step);
    return takeX86IntegerOperand(encoder, step.operation, 0, cursor);
}

static void moveX86IntegerRegister(CCallHelpers& jit, Width width, GPRReg source, GPRReg destination)
{
    if (source == destination)
        return;
    if (width == Width32)
        jit.zeroExtend32ToWord(source, destination);
    else
        jit.move(source, destination);
}

static void moveX86IntegerOperand(CCallHelpers& jit, Width width, const X86IntegerOperand& source, GPRReg destination)
{
    switch (source.kind) {
    case X86IntegerOperand::InRegister:
        moveX86IntegerRegister(jit, width, source.gpr, destination);
        return;
    case X86IntegerOperand::AtAddress:
    case X86IntegerOperand::AtIndexedAddress:
        withX86IntegerAddress(source, [&](auto address) {
            if (width == Width32)
                jit.load32(address, destination);
            else
                jit.load64(address, destination);
        });
        return;
    case X86IntegerOperand::IsConstant:
        if (width == Width32)
            jit.move(MacroAssembler::TrustedImm32(static_cast<int32_t>(source.constant)), destination);
        else
            jit.move(MacroAssembler::TrustedImm64(source.constant), destination);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static GPRReg bringX86IntegerOperandIntoRegister(CCallHelpers& jit, Width width, const X86IntegerOperand& operand, GPRReg destination)
{
    if (operand.isInRegister())
        return operand.gpr;
    moveX86IntegerOperand(jit, width, operand, destination);
    return destination;
}

static bool hasX86IntegerMemoryForm(X86IntegerInstruction instruction, Width width, const X86IntegerOperand& operand)
{
    if (instruction != X86IntegerInstruction::Multiply)
        return true;
    return width == Width32 && operand.kind == X86IntegerOperand::AtAddress;
}

template<typename Source>
static void applyX86IntegerInstruction(CCallHelpers& jit, X86IntegerInstruction instruction, Width width, Source source, GPRReg destination)
{
    bool is32 = width == Width32;
    switch (instruction) {
    case X86IntegerInstruction::Add:
        if (is32)
            jit.add32(source, destination);
        else
            jit.add64(source, destination);
        return;
    case X86IntegerInstruction::Subtract:
        if (is32)
            jit.sub32(source, destination);
        else
            jit.sub64(source, destination);
        return;
    case X86IntegerInstruction::Multiply:
        if constexpr (std::is_same_v<Source, GPRReg>) {
            if (is32)
                jit.mul32(source, destination);
            else
                jit.mul64(source, destination);
        } else if constexpr (std::is_same_v<Source, MacroAssembler::Address>) {
            RELEASE_ASSERT(is32);
            jit.mul32(source, destination);
        } else
            RELEASE_ASSERT_NOT_REACHED();
        return;
    case X86IntegerInstruction::And:
        if (is32)
            jit.and32(source, destination);
        else
            jit.and64(source, destination);
        return;
    case X86IntegerInstruction::Or:
        if (is32)
            jit.or32(source, destination);
        else
            jit.or64(source, destination);
        return;
    case X86IntegerInstruction::ExclusiveOr:
        if (is32)
            jit.xor32(source, destination);
        else
            jit.xor64(source, destination);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ALWAYS_INLINE void applyX86IntegerImmediate(CCallHelpers& jit, X86IntegerInstruction instruction, Width width, int64_t constant, GPRReg source, GPRReg destination)
{
    bool is32 = width == Width32;
    RELEASE_ASSERT(is32 || isRepresentableAs<int32_t>(constant));
    MacroAssembler::TrustedImm32 immediate(static_cast<int32_t>(constant));
    switch (instruction) {
    case X86IntegerInstruction::Add:
        if (is32)
            jit.add32(immediate, source, destination);
        else if (source == destination)
            jit.add64(immediate, destination);
        else
            jit.add64(immediate, source, destination);
        return;
    case X86IntegerInstruction::Subtract:
        if (is32)
            jit.sub32(source, immediate, destination);
        else
            jit.sub64(source, immediate, destination);
        return;
    case X86IntegerInstruction::Multiply:
        if (is32)
            jit.mul32(immediate, source, destination);
        else
            jit.mul64(immediate, source, destination);
        return;
    case X86IntegerInstruction::And:
        if (is32)
            jit.and32(immediate, source, destination);
        else
            jit.and64(immediate, source, destination);
        return;
    case X86IntegerInstruction::Or:
        if (is32)
            jit.or32(immediate, source, destination);
        else
            jit.or64(immediate, source, destination);
        return;
    case X86IntegerInstruction::ExclusiveOr:
        if (is32)
            jit.xor32(immediate, source, destination);
        else
            jit.xor64(immediate, source, destination);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void applyX86IntegerOperand(CCallHelpers& jit, X86IntegerInstruction instruction, Width width, const X86IntegerOperand& source, GPRReg destination)
{
    switch (source.kind) {
    case X86IntegerOperand::InRegister:
        applyX86IntegerInstruction(jit, instruction, width, source.gpr, destination);
        return;
    case X86IntegerOperand::AtAddress:
    case X86IntegerOperand::AtIndexedAddress:
        withX86IntegerAddress(source, [&](auto address) {
            applyX86IntegerInstruction(jit, instruction, width, address, destination);
        });
        return;
    case X86IntegerOperand::IsConstant:
        applyX86IntegerImmediate(jit, instruction, width, source.constant, destination, destination);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void encodeX86IntegerBinary(Encoder& encoder, const Step& step, X86IntegerInstruction instruction)
{
    CCallHelpers& jit = encoder.m_jit;
    AllowMacroScratchRegisterUsage allowScratch(jit);
    const Operation* operation = step.operation;
    Width width = widthOf(operation->type());
    GPRReg destination = step.destination.gpr();
    bool isCommutative = instruction != X86IntegerInstruction::Subtract;

    LeafCursor cursor(step);
    X86IntegerOperand left = takeX86IntegerOperand(encoder, operation, 0, cursor);
    X86IntegerOperand right = takeX86IntegerOperand(encoder, operation, 1, cursor);
    RELEASE_ASSERT(!left.isMemory() || !right.isMemory());

    for (X86IntegerOperand* operand : { &left, &right }) {
        if (operand->isMemory() && !hasX86IntegerMemoryForm(instruction, width, *operand)) {
            moveX86IntegerOperand(jit, width, *operand, scratchGPR);
            *operand = X86IntegerOperand::inRegister(scratchGPR);
        }
    }
    RELEASE_ASSERT(!left.mentions(scratchGPR) || !right.mentions(scratchGPR));

    if (isCommutative && right.isInRegister() && (!left.isInRegister() || right.gpr == destination))
        std::swap(left, right);

    if (!isCommutative && right.isInRegister() && right.gpr == destination && !left.mentions(destination)) {
        if (width == Width32)
            jit.neg32(destination);
        else
            jit.neg64(destination);
        applyX86IntegerOperand(jit, X86IntegerInstruction::Add, width, left, destination);
        return;
    }

    if (!left.isInRegister()) {
        GPRReg target = right.mentions(destination) ? scratchGPR : destination;
        moveX86IntegerOperand(jit, width, left, target);
        applyX86IntegerOperand(jit, instruction, width, right, target);
        moveX86IntegerRegister(jit, width, target, destination);
        return;
    }

    if (right.isConstant()) {
        applyX86IntegerImmediate(jit, instruction, width, right.constant, left.gpr, destination);
        return;
    }

    if (left.gpr == destination) {
        applyX86IntegerOperand(jit, instruction, width, right, destination);
        return;
    }

    if (!right.mentions(destination)) {
        if (instruction == X86IntegerInstruction::Add && right.isInRegister()) {
            GPRReg base = left.gpr;
            GPRReg index = right.gpr;
            if (index == MacroAssembler::stackPointerRegister)
                std::swap(base, index);
            if (width == Width32)
                jit.add32(base, index, destination);
            else
                jit.add64(base, index, destination);
            return;
        }
        moveX86IntegerRegister(jit, width, left.gpr, destination);
        applyX86IntegerOperand(jit, instruction, width, right, destination);
        return;
    }

    RELEASE_ASSERT(right.isMemory());
    if (isCommutative) {
        moveX86IntegerOperand(jit, width, right, destination);
        applyX86IntegerInstruction(jit, instruction, width, left.gpr, destination);
        return;
    }
    RELEASE_ASSERT(left.gpr != scratchGPR);
    moveX86IntegerOperand(jit, width, right, scratchGPR);
    moveX86IntegerRegister(jit, width, left.gpr, destination);
    applyX86IntegerInstruction(jit, instruction, width, scratchGPR, destination);
}

static void encodeX86IntegerShift(Encoder& encoder, const Step& step, X86IntegerShift shift)
{
    CCallHelpers& jit = encoder.m_jit;
    const Operation* operation = step.operation;
    Width width = widthOf(operation->type());
    bool is32 = width == Width32;
    GPRReg destination = step.destination.gpr();

    RELEASE_ASSERT(operation->child(1)->hasInt32());
    int32_t amount = operation->child(1)->asInt32() & (is32 ? 31 : 63);
    MacroAssembler::TrustedImm32 immediate(amount);

    GPRReg source = bringX86IntegerOperandIntoRegister(jit, width, takeFirstX86IntegerOperand(encoder, step), destination);
    switch (shift) {
    case X86IntegerShift::Left:
        if (amount == 1) {
            if (is32)
                jit.add32(source, source, destination);
            else
                jit.add64(source, source, destination);
            return;
        }
        if (is32)
            jit.lshift32(source, immediate, destination);
        else
            jit.lshift64(source, immediate, destination);
        return;
    case X86IntegerShift::ArithmeticRight:
        if (is32)
            jit.rshift32(source, immediate, destination);
        else
            jit.rshift64(source, immediate, destination);
        return;
    case X86IntegerShift::LogicalRight:
        if (is32)
            jit.urshift32(source, immediate, destination);
        else
            jit.urshift64(source, immediate, destination);
        return;
    case X86IntegerShift::RotateRight:
        if (is32)
            jit.rotateRight32(source, immediate, destination);
        else
            jit.rotateRight64(source, immediate, destination);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void encodeX86IntegerExtension(Encoder& encoder, const Step& step, X86IntegerExtension extension)
{
    CCallHelpers& jit = encoder.m_jit;
    AllowMacroScratchRegisterUsage allowScratch(jit);
    bool to32 = widthOf(step.operation->type()) == Width32;
    GPRReg destination = step.destination.gpr();

    X86IntegerOperand source = takeFirstX86IntegerOperand(encoder, step);
    if (source.isConstant()) {
        moveX86IntegerOperand(jit, Width32, source, destination);
        source = X86IntegerOperand::inRegister(destination);
    }
    if (source.isInRegister() && source.gpr == destination && isFreeWhenCoalesced(encoder.m_liveness, step.operation))
        return;

    switch (extension) {
    case X86IntegerExtension::SignExtend8:
        if (source.isInRegister()) {
            if (to32)
                jit.signExtend8To32(source.gpr, destination);
            else
                jit.signExtend8To64(source.gpr, destination);
            return;
        }
        withX86IntegerAddress(source, [&](auto address) {
            if (to32)
                jit.load8SignedExtendTo32(address, destination);
            else
                jit.load8SignedExtendTo64(address, destination);
        });
        return;
    case X86IntegerExtension::SignExtend16:
        if (source.isInRegister()) {
            if (to32)
                jit.signExtend16To32(source.gpr, destination);
            else
                jit.signExtend16To64(source.gpr, destination);
            return;
        }
        withX86IntegerAddress(source, [&](auto address) {
            if (to32)
                jit.load16SignedExtendTo32(address, destination);
            else
                jit.load16SignedExtendTo64(address, destination);
        });
        return;
    case X86IntegerExtension::SignExtend32:
        ASSERT(!to32);
        if (source.isInRegister()) {
            jit.signExtend32To64(source.gpr, destination);
            return;
        }
        withX86IntegerAddress(source, [&](auto address) {
            jit.load32SignedExtendTo64(address, destination);
        });
        return;
    case X86IntegerExtension::ZeroExtend32:
        ASSERT(!to32);
        if (source.isInRegister()) {
            jit.zeroExtend32ToWord(source.gpr, destination);
            return;
        }
        withX86IntegerAddress(source, [&](auto address) {
            jit.load32(address, destination);
        });
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void encodeX86IntegerMove(Encoder& encoder, const Step& step)
{
    const Operation* child = inlineChildWithoutCheck(step.operation, 0);
    if (!encoder.isFused(child) && step.sources[0].isImmediate()) {
        encoder.encodeMaterialize(child, step.destination.reg());
        return;
    }
    moveX86IntegerOperand(encoder.m_jit, widthOf(child->type()), takeFirstX86IntegerOperand(encoder, step), step.destination.gpr());
}

static void encodeX86IntegerNegation(Encoder& encoder, const Step& step)
{
    CCallHelpers& jit = encoder.m_jit;
    Width width = widthOf(step.operation->type());
    GPRReg destination = step.destination.gpr();
    GPRReg source = bringX86IntegerOperandIntoRegister(jit, width, takeFirstX86IntegerOperand(encoder, step), destination);
    if (width == Width32)
        jit.neg32(source, destination);
    else
        jit.neg64(source, destination);
}

static void encodeX86LeadingZeroCount(Encoder& encoder, const Step& step)
{
    CCallHelpers& jit = encoder.m_jit;
    Width width = widthOf(step.operation->type());
    GPRReg destination = step.destination.gpr();
    X86IntegerOperand source = takeFirstX86IntegerOperand(encoder, step);

    if (source.kind == X86IntegerOperand::AtAddress) {
        MacroAssembler::Address address(source.gpr, source.offset);
        if (width == Width32)
            jit.countLeadingZeros32(address, destination);
        else
            jit.countLeadingZeros64(address, destination);
        return;
    }
    GPRReg gpr = bringX86IntegerOperandIntoRegister(jit, width, source, destination);
    if (width == Width32)
        jit.countLeadingZeros32(gpr, destination);
    else
        jit.countLeadingZeros64(gpr, destination);
}

static void encodeX86PopulationCountWithoutPOPCNT(Encoder& encoder, const Step& step, Width width, const X86IntegerOperand& source, GPRReg destination)
{
    CCallHelpers& jit = encoder.m_jit;
    RELEASE_ASSERT(step.patchAssignment != UINT32_MAX);
    RegisterMask scratchRegisters = encoder.m_allocation.patchAssignment(step.patchAssignment).gpScratch;
    RELEASE_ASSERT(scratchRegisters);
    GPRReg shifted = Reg::fromIndex(static_cast<unsigned>(std::countr_zero(scratchRegisters))).gpr();
    RELEASE_ASSERT(shifted != destination && shifted != scratchGPR && !source.mentions(shifted));

    moveX86IntegerOperand(jit, width, source, destination);
    if (width == Width32) {
        jit.urshift32(destination, MacroAssembler::TrustedImm32(1), shifted);
        jit.and32(MacroAssembler::TrustedImm32(0x55555555), shifted);
        jit.sub32(shifted, destination);
        jit.urshift32(destination, MacroAssembler::TrustedImm32(2), shifted);
        jit.and32(MacroAssembler::TrustedImm32(0x33333333), destination);
        jit.and32(MacroAssembler::TrustedImm32(0x33333333), shifted);
        jit.add32(shifted, destination);
        jit.urshift32(destination, MacroAssembler::TrustedImm32(4), shifted);
        jit.add32(shifted, destination);
        jit.and32(MacroAssembler::TrustedImm32(0x0f0f0f0f), destination);
        jit.mul32(MacroAssembler::TrustedImm32(0x01010101), destination, destination);
        jit.urshift32(MacroAssembler::TrustedImm32(24), destination);
        return;
    }

    AllowMacroScratchRegisterUsage allowScratch(jit);
    jit.urshift64(destination, MacroAssembler::TrustedImm32(1), shifted);
    jit.move(MacroAssembler::TrustedImm64(0x5555555555555555), scratchGPR);
    jit.and64(scratchGPR, shifted);
    jit.sub64(shifted, destination);
    jit.urshift64(destination, MacroAssembler::TrustedImm32(2), shifted);
    jit.move(MacroAssembler::TrustedImm64(0x3333333333333333), scratchGPR);
    jit.and64(scratchGPR, destination);
    jit.and64(scratchGPR, shifted);
    jit.add64(shifted, destination);
    jit.urshift64(destination, MacroAssembler::TrustedImm32(4), shifted);
    jit.add64(shifted, destination);
    jit.move(MacroAssembler::TrustedImm64(0x0f0f0f0f0f0f0f0f), scratchGPR);
    jit.and64(scratchGPR, destination);
    jit.move(MacroAssembler::TrustedImm64(0x0101010101010101), scratchGPR);
    jit.mul64(scratchGPR, destination);
    jit.urshift64(MacroAssembler::TrustedImm32(56), destination);
}

static void encodeX86PopulationCount(Encoder& encoder, const Step& step)
{
    CCallHelpers& jit = encoder.m_jit;
    Width width = widthOf(step.operation->type());
    GPRReg destination = step.destination.gpr();
    X86IntegerOperand source = takeFirstX86IntegerOperand(encoder, step);

    if (!MacroAssembler::supportsCountPopulation(encoder.m_proc.traits().features)) {
        encodeX86PopulationCountWithoutPOPCNT(encoder, step, width, source, destination);
        return;
    }

    if (source.kind == X86IntegerOperand::AtAddress) {
        MacroAssembler::Address address(source.gpr, source.offset);
        if (width == Width32)
            jit.countPopulation32(address, destination);
        else
            jit.countPopulation64(address, destination);
        return;
    }
    GPRReg gpr = bringX86IntegerOperandIntoRegister(jit, width, source, destination);
    if (width == Width32)
        jit.countPopulation32(gpr, destination);
    else
        jit.countPopulation64(gpr, destination);
}

static ALWAYS_INLINE void encodeX86ScaledSum(Encoder& encoder, const Step& step)
{
    LeafCursor cursor(step);
    AddressForm form = encoder.resolveScaledSum(step.operation, cursor);
    RELEASE_ASSERT(form.index != MacroAssembler::stackPointerRegister);
    encoder.m_jit.getEffectiveAddress(MacroAssembler::BaseIndex(form.base, form.index, static_cast<MacroAssembler::Scale>(form.shift), form.offset), step.destination.gpr());
}

bool Encoder::tryEncodeSumAndConstant(const Step& sumStep, const Step& outerStep)
{
    ASSERT(sumStep.kind == StepKind::Execute && sumStep.operation->opcode() == Add);
    if (outerStep.kind != StepKind::Execute || outerStep.operation->opcode() != Add)
        return false;
    const Operation* sum = sumStep.operation;
    const Operation* outer = outerStep.operation;
    if (sum->type() != Int32 || outer->child(0) != sum || !outer->child(1)->hasInt32())
        return false;
    if (sum->useCount() != 1 || sum->isNarrowReference() || m_liveness.of(sum).has(ValueFlag::IsKeptForCollector))
        return false;
    if (m_allocation.isSpilled(sum) || m_liveness.worksInMemory(sum) || m_liveness.worksInMemory(outer) || m_liveness.usesOverflowFlag(outer))
        return false;
    for (const Operation* child : sum->children()) {
        if (child->isConstant() || (m_liveness.isFused(child) && child->opcode() != Trunc))
            return false;
    }

    Location base = sumStep.sources[0];
    Location index = sumStep.sources[1];
    if (!base.isRegister() || !index.isRegister() || !sumStep.destination.isRegister() || !outerStep.destination.isRegister())
        return false;
    if (outerStep.sources[0] != sumStep.destination || outerStep.sources[1].kind() != Location::Immediate)
        return false;
    if (base.gpr() == MacroAssembler::stackPointerRegister || index.gpr() == MacroAssembler::stackPointerRegister)
        return false;

    m_jit.x86Lea32(MacroAssembler::BaseIndex(base.gpr(), index.gpr(), MacroAssembler::TimesOne, outer->child(1)->asInt32()), outerStep.destination.gpr());
    return true;
}

static X86IntegerOperand x86OperandOfSumThatSetsFlags(Encoder& encoder, Location location, const Operation* operand)
{
    switch (location.kind()) {
    case Location::Register:
        return X86IntegerOperand::inRegister(location.gpr());
    case Location::Spill:
    case Location::Outgoing:
    case Location::InstanceWord:
        return X86IntegerOperand::at(encoder.addressOf(location));
    case Location::Immediate:
        RELEASE_ASSERT(operand->hasInt() && isRepresentableAs<int32_t>(operand->asInt()));
        return X86IntegerOperand::forConstant(operand->asInt());
    case Location::None:
    case Location::SlotBytes:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void applyX86InstructionThatSetsFlags(CCallHelpers& jit, bool subtracts, const X86IntegerOperand& source, GPRReg destination)
{
    switch (source.kind) {
    case X86IntegerOperand::InRegister:
        if (subtracts)
            jit.m_assembler.subl_rr(source.gpr, destination);
        else
            jit.m_assembler.addl_rr(source.gpr, destination);
        return;
    case X86IntegerOperand::AtAddress:
        if (subtracts)
            jit.m_assembler.subl_mr(source.offset, source.gpr, destination);
        else
            jit.m_assembler.addl_mr(source.offset, source.gpr, destination);
        return;
    case X86IntegerOperand::IsConstant:
        if (subtracts)
            jit.m_assembler.subl_ir(static_cast<int32_t>(source.constant), destination);
        else
            jit.m_assembler.addl_ir(static_cast<int32_t>(source.constant), destination);
        return;
    case X86IntegerOperand::AtIndexedAddress:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void Encoder::encodeSumThatSetsFlags(const Step& step)
{
    OverflowCheckedSum test = overflowCheckedSumOf(m_liveness, step.operation);
    AllowMacroScratchRegisterUsage allowScratch(m_jit);
    GPRReg destination = step.destination.gpr();
    bool subtracts = test.subtracts();
    X86IntegerOperand left = x86OperandOfSumThatSetsFlags(*this, step.sources[0], test.left());
    X86IntegerOperand right = x86OperandOfSumThatSetsFlags(*this, step.sources[1], test.right());
    RELEASE_ASSERT(!left.isConstant() && (!left.isMemory() || !right.isMemory()));

    if (!subtracts && right.isInRegister() && (!left.isInRegister() || right.gpr == destination))
        std::swap(left, right);

    GPRReg target = destination;
    if (!left.isInRegister() || left.gpr != destination) {
        if (right.mentions(destination))
            target = scratchGPR;
        moveX86IntegerOperand(m_jit, Width32, left, target);
    }
    applyX86InstructionThatSetsFlags(m_jit, subtracts, right, target);
    moveX86IntegerRegister(m_jit, Width32, target, destination);
}

MacroAssembler::Jump Encoder::jumpOnOverflow(bool whenItOverflowed)
{
    return MacroAssembler::Jump(m_jit.m_assembler.jCC(whenItOverflowed ? X86Assembler::ConditionO : X86Assembler::ConditionNO));
}

void Encoder::encodeInteger(const Step& step)
{
    RELEASE_ASSERT(step.kind == StepKind::Execute && step.destination.isRegister());
    ASSERT(step.operation->type().isInt());

    switch (step.operation->opcode()) {
    case Add:
        if (m_liveness.usesOverflowFlag(step.operation))
            encodeSumThatSetsFlags(step);
        else if (addsScaledIndex(m_liveness, step.operation))
            encodeX86ScaledSum(*this, step);
        else
            encodeX86IntegerBinary(*this, step, X86IntegerInstruction::Add);
        return;
    case Sub:
        if (m_liveness.usesOverflowFlag(step.operation))
            encodeSumThatSetsFlags(step);
        else
            encodeX86IntegerBinary(*this, step, X86IntegerInstruction::Subtract);
        return;
    case Mul:
        encodeX86IntegerBinary(*this, step, X86IntegerInstruction::Multiply);
        return;
    case BitAnd:
        encodeX86IntegerBinary(*this, step, X86IntegerInstruction::And);
        return;
    case BitOr:
        encodeX86IntegerBinary(*this, step, X86IntegerInstruction::Or);
        return;
    case BitXor:
        encodeX86IntegerBinary(*this, step, X86IntegerInstruction::ExclusiveOr);
        return;

    case Shl:
        encodeX86IntegerShift(*this, step, X86IntegerShift::Left);
        return;
    case SShr:
        encodeX86IntegerShift(*this, step, X86IntegerShift::ArithmeticRight);
        return;
    case ZShr:
        encodeX86IntegerShift(*this, step, X86IntegerShift::LogicalRight);
        return;
    case RotR:
        encodeX86IntegerShift(*this, step, X86IntegerShift::RotateRight);
        return;

    case SExt8:
        encodeX86IntegerExtension(*this, step, X86IntegerExtension::SignExtend8);
        return;
    case SExt16:
        encodeX86IntegerExtension(*this, step, X86IntegerExtension::SignExtend16);
        return;
    case SExt32:
        encodeX86IntegerExtension(*this, step, X86IntegerExtension::SignExtend32);
        return;
    case ZExt32:
        encodeX86IntegerExtension(*this, step, X86IntegerExtension::ZeroExtend32);
        return;

    case Trunc:
    case Opaque:
        encodeX86IntegerMove(*this, step);
        return;
    case Neg:
        encodeX86IntegerNegation(*this, step);
        return;
    case Clz:
        encodeX86LeadingZeroCount(*this, step);
        return;
    case PopCount:
        encodeX86PopulationCount(*this, step);
        return;

    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT) && CPU(X86_64)
