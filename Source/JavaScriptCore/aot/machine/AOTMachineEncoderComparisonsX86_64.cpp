/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineEncoder.h"

#if ENABLE(AOT) && CPU(X86_64)

#include "AllowMacroScratchRegisterUsage.h"
#include <array>
#include <bit>
#include <optional>
#include <span>
#include <utility>

namespace JSC { namespace AOT { namespace Machine {

struct X86ConditionsOfComparison {
    std::optional<MacroAssembler::RelationalCondition> ofIntegers;
    std::optional<MacroAssembler::DoubleCondition> ofNumbers;
};

static ALWAYS_INLINE X86ConditionsOfComparison conditionsOfX86Comparison(Opcode relation)
{
    switch (relation) {
    case Equal:
        return { MacroAssembler::Equal, MacroAssembler::DoubleEqualAndOrdered };
    case NotEqual:
        return { MacroAssembler::NotEqual, MacroAssembler::DoubleNotEqualOrUnordered };
    case LessThan:
        return { MacroAssembler::LessThan, MacroAssembler::DoubleLessThanAndOrdered };
    case GreaterThan:
        return { MacroAssembler::GreaterThan, MacroAssembler::DoubleGreaterThanAndOrdered };
    case LessEqual:
        return { MacroAssembler::LessThanOrEqual, MacroAssembler::DoubleLessThanOrEqualAndOrdered };
    case GreaterEqual:
        return { MacroAssembler::GreaterThanOrEqual, MacroAssembler::DoubleGreaterThanOrEqualAndOrdered };
    case Above:
        return { MacroAssembler::Above, std::nullopt };
    case Below:
        return { MacroAssembler::Below, std::nullopt };
    case AboveEqual:
        return { MacroAssembler::AboveOrEqual, std::nullopt };
    case BelowEqual:
        return { MacroAssembler::BelowOrEqual, std::nullopt };
    case EqualOrUnordered:
        return { std::nullopt, MacroAssembler::DoubleEqualOrUnordered };

    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

struct X86ComparedOperand {
    enum Kind : uint8_t {
        InRegister,
        InMemory,
        IsConstant,
    };

    static X86ComparedOperand inRegister(Reg reg)
    {
        X86ComparedOperand result;
        result.kind = InRegister;
        result.reg = reg;
        return result;
    }

    static X86ComparedOperand inMemory(MacroAssembler::Address address)
    {
        X86ComparedOperand result;
        result.kind = InMemory;
        result.base = address.base;
        result.offset = address.offset;
        return result;
    }

    static ALWAYS_INLINE X86ComparedOperand readBy(Opcode load, const AddressForm& form)
    {
        RELEASE_ASSERT(isLoad(load) && form.kind != AddressForm::InstanceSymbol && form.extension == IndexExtension::None && form.shift <= 3);
        X86ComparedOperand result;
        result.kind = InMemory;
        result.load = load;
        result.base = form.base;
        result.offset = form.offset;
        if (form.kind == AddressForm::BaseIndex) {
            result.index = form.index;
            result.shift = form.shift;
        }
        return result;
    }

    static X86ComparedOperand forConstant(int64_t constant)
    {
        X86ComparedOperand result;
        result.kind = IsConstant;
        result.constant = constant;
        return result;
    }

    bool isInRegister() const { return kind == InRegister; }
    bool isInMemory() const { return kind == InMemory; }
    bool isConstant() const { return kind == IsConstant; }
    bool isWholeWordAtBase() const { return isInMemory() && load == Load && index == InvalidGPRReg; }

    GPRReg gpr() const
    {
        ASSERT(isInRegister());
        return reg.gpr();
    }
    FPRReg fpr() const
    {
        ASSERT(isInRegister());
        return reg.fpr();
    }
    MacroAssembler::Address address() const
    {
        RELEASE_ASSERT(isWholeWordAtBase());
        return MacroAssembler::Address(base, offset);
    }

    bool mentions(Reg other) const
    {
        switch (kind) {
        case InRegister:
            return reg == other;
        case InMemory:
            return Reg(base) == other || (index != InvalidGPRReg && Reg(index) == other);
        case IsConstant:
            return false;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    Kind kind { IsConstant };
    uint8_t shift { 0 };
    bool offsetIsFilledLater { false };
    Opcode load { Load };
    Reg reg;
    GPRReg base { InvalidGPRReg };
    GPRReg index { InvalidGPRReg };
    int32_t offset { 0 };
    int64_t constant { 0 };
};

struct X86Comparison {
    Test::Kind kind;
    Opcode relation;
    Width width;
    X86ComparedOperand left;
    X86ComparedOperand right;
    bool rightIsNumberOfBit { false };
    const Operation* linkedNumberOnRight { nullptr };
};

static X86ComparedOperand x86ComparedOperandAt(const Encoder& encoder, Location location, int64_t constant)
{
    switch (location.kind()) {
    case Location::Register:
        return X86ComparedOperand::inRegister(location.reg());
    case Location::Spill:
    case Location::Outgoing:
    case Location::InstanceWord:
        return X86ComparedOperand::inMemory(encoder.addressOf(location));
    case Location::Immediate:
        return X86ComparedOperand::forConstant(constant);
    case Location::None:
    case Location::SlotBytes:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static X86ComparedOperand x86ComparedOperandReadByFusedLoad(Encoder& encoder, const Operation* load, AddressForm form)
{
    RELEASE_ASSERT(isLoad(load->opcode()) && !load->traps() && !encoder.m_liveness.of(load).has(ValueFlag::TakesOverTrap));
    CCallHelpers& jit = encoder.m_jit;

    if (form.kind == AddressForm::InstanceSymbol) {
        encoder.m_relocations.moveInstanceAddress(form.symbol, instanceGPR, scratchGPR);
        form.kind = AddressForm::BaseOffset;
        form.base = scratchGPR;
    }
    if (load->type().isInt())
        return X86ComparedOperand::readBy(load->opcode(), form);

    RELEASE_ASSERT(load->opcode() == Load && load->type().isFloat());
    if (form.kind == AddressForm::BaseOffset)
        return X86ComparedOperand::inMemory(form.address());
    RELEASE_ASSERT(form.extension == IndexExtension::None && form.shift <= 3);
    MacroAssembler::BaseIndex address(form.base, form.index, static_cast<MacroAssembler::Scale>(form.shift), form.offset);
    if (load->type() == Double)
        jit.loadDouble(address, scratchFPR);
    else
        jit.loadFloat(address, scratchFPR);
    return X86ComparedOperand::inRegister(scratchFPR);
}

static ALWAYS_INLINE X86Comparison x86ComparisonOfTest(Encoder& encoder, const Test& test)
{
    RELEASE_ASSERT(!test.left.isImmediate());
    X86ComparedOperand left = test.loadOnLeft ? x86ComparedOperandReadByFusedLoad(encoder, test.loadOnLeft, test.addressOnLeft) : x86ComparedOperandAt(encoder, test.left, 0);
    return X86Comparison { test.kind, test.relation, test.width, left, x86ComparedOperandAt(encoder, test.right, test.rightConstant), test.rightIsNumberOfBit, test.linkedNumberOnRight };
}

static X86ComparedOperand takeX86ComparedOperand(Encoder& encoder, const Operation* comparison, unsigned childIndex, LeafCursor& cursor)
{
    const Operation* child = comparison->child(childIndex);
    if (encoder.m_liveness.isFused(child))
        return x86ComparedOperandReadByFusedLoad(encoder, child, encoder.resolveAddress(child, cursor));

    Location location = cursor.take();
    if (!location.isImmediate())
        return x86ComparedOperandAt(encoder, location, 0);
    if (child->isConstant())
        return X86ComparedOperand::forConstant(child->hasInt() ? child->asInt() : child->constantBits());

    RELEASE_ASSERT(child->type().isInt());
    encoder.encodeMaterialize(child, scratchGPR);
    return X86ComparedOperand::inRegister(scratchGPR);
}

static X86Comparison takeX86Comparison(Encoder& encoder, const Operation* operation, LeafCursor& cursor)
{
    X86ComparedOperand left = takeX86ComparedOperand(encoder, operation, 0, cursor);
    const Operation* rightChild = operation->child(1);
    bool comparesLinkedNumber = rightChild->opcode() == NumberOf && cursor.peek().isImmediate();
    if (comparesLinkedNumber)
        cursor.take();
    X86ComparedOperand right = comparesLinkedNumber ? X86ComparedOperand::forConstant(0) : takeX86ComparedOperand(encoder, operation, 1, cursor);
    RELEASE_ASSERT(!left.mentions(scratchGPR) || !right.mentions(scratchGPR));
    RELEASE_ASSERT(!left.mentions(scratchFPR) || !right.mentions(scratchFPR));

    Type type = operation->child(0)->type();
    return X86Comparison { type.isFloat() ? Test::CompareNumbers : Test::CompareIntegers, operation->opcode(), widthOf(type), left, right, false, comparesLinkedNumber ? rightChild : nullptr };
}

static void bringX86ComparedIntegerIntoScratch(CCallHelpers& jit, Width width, X86ComparedOperand& operand, const X86ComparedOperand& other)
{
    RELEASE_ASSERT(!operand.isInRegister() && !other.mentions(scratchGPR));
    bool is32 = width == Width32;
    if (operand.isInMemory()) {
        auto loadFrom = [&](auto address) {
            if (operand.load == Load8Z)
                jit.load8(address, scratchGPR);
            else if (operand.load == Load8S)
                jit.load8SignedExtendTo32(address, scratchGPR);
            else if (operand.load == Load16Z)
                jit.load16(address, scratchGPR);
            else if (operand.load == Load16S)
                jit.load16SignedExtendTo32(address, scratchGPR);
            else if (is32)
                jit.load32(address, scratchGPR);
            else
                jit.load64(address, scratchGPR);
        };
        if (operand.index == InvalidGPRReg)
            loadFrom(MacroAssembler::Address(operand.base, operand.offset));
        else
            loadFrom(MacroAssembler::BaseIndex(operand.base, operand.index, static_cast<MacroAssembler::Scale>(operand.shift), operand.offset));
    } else if (is32)
        jit.move(MacroAssembler::TrustedImm32(static_cast<int32_t>(operand.constant)), scratchGPR);
    else
        jit.move(MacroAssembler::TrustedImm64(operand.constant), scratchGPR);
    operand = X86ComparedOperand::inRegister(scratchGPR);
}

static ALWAYS_INLINE MacroAssembler::RelationalCondition unsignedFormOfX86Condition(MacroAssembler::RelationalCondition condition)
{
    switch (condition) {
    case MacroAssembler::LessThan:
        return MacroAssembler::Below;
    case MacroAssembler::LessThanOrEqual:
        return MacroAssembler::BelowOrEqual;
    case MacroAssembler::GreaterThan:
        return MacroAssembler::Above;
    case MacroAssembler::GreaterThanOrEqual:
        return MacroAssembler::AboveOrEqual;
    case MacroAssembler::Equal:
    case MacroAssembler::NotEqual:
    case MacroAssembler::Above:
    case MacroAssembler::AboveOrEqual:
    case MacroAssembler::Below:
    case MacroAssembler::BelowOrEqual:
        return condition;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ALWAYS_INLINE std::optional<MacroAssembler::RelationalCondition> tryEmitFlagsOfX86WordInMemory(CCallHelpers& jit, Test::Kind kind, const X86ComparedOperand& word, int64_t constant, Width width, MacroAssembler::RelationalCondition condition)
{
    ASSERT(word.isInMemory());
    X86Assembler& assembler = jit.m_assembler;
    bool isIndexed = word.index != InvalidGPRReg;
    bool isByte = word.load == Load8Z || word.load == Load8S;
    bool isHalf = word.load == Load16Z || word.load == Load16S;
    bool extendsSign = word.load == Load8S || word.load == Load16S;
    unsigned numberOfBytes = isByte ? 1 : isHalf ? 2 : width == Width32 ? 4 : 8;

    auto compareWith = [&](int32_t immediate) ALWAYS_INLINE_LAMBDA {
        if (isByte)
            isIndexed ? assembler.cmpb_im(immediate, word.offset, word.base, word.index, word.shift) : assembler.cmpb_im(immediate, word.offset, word.base);
        else if (isHalf)
            isIndexed ? assembler.cmpw_im(immediate, word.offset, word.base, word.index, word.shift) : assembler.cmpw_im(immediate, word.offset, word.base);
        else if (numberOfBytes == 4)
            isIndexed ? assembler.cmpl_im(immediate, word.offset, word.base, word.index, word.shift) : assembler.cmpl_im(immediate, word.offset, word.base);
        else
            isIndexed ? assembler.cmpq_im(immediate, word.offset, word.base, word.index, word.shift) : assembler.cmpq_im(immediate, word.offset, word.base);
    };

    if (kind == Test::CompareIntegers) {
        if (isByte || isHalf) {
            int64_t least = extendsSign ? -128 : 0;
            int64_t most = isByte && !extendsSign ? 255 : 127;
            if (constant < least || constant > most)
                return std::nullopt;
            if (!extendsSign)
                condition = unsignedFormOfX86Condition(condition);
        }
        compareWith(static_cast<int32_t>(constant));
        return condition;
    }

    ASSERT(kind == Test::TestBits);
    if (constant == -1) {
        compareWith(0);
        return condition;
    }

    uint64_t bits = static_cast<uint64_t>(constant);
    if (numberOfBytes < 8) {
        uint64_t bitsOfWord = (uint64_t { 1 } << (8 * numberOfBytes)) - 1;
        uint64_t bitsOfRegister = width == Width32 ? uint64_t { UINT32_MAX } : UINT64_MAX;
        if (extendsSign && (bits & bitsOfRegister & ~bitsOfWord))
            return std::nullopt;
        bits &= bitsOfWord;
    }
    for (unsigned byte = 0; byte < numberOfBytes; ++byte) {
        if ((bits & ~(uint64_t { 0xff } << (8 * byte))) || word.offset > INT32_MAX - 7 || (byte && word.offsetIsFilledLater))
            continue;
        int32_t mask = static_cast<int32_t>(bits >> (8 * byte));
        int32_t offset = word.offset + static_cast<int32_t>(byte);
        isIndexed ? assembler.testb_im(mask, offset, word.base, word.index, word.shift) : assembler.testb_im(mask, offset, word.base);
        return condition;
    }
    if (isHalf)
        return std::nullopt;
    if (numberOfBytes == 4 || bits <= UINT32_MAX) {
        int32_t mask = static_cast<int32_t>(static_cast<uint32_t>(bits));
        isIndexed ? assembler.testl_i32m(mask, word.offset, word.base, word.index, word.shift) : assembler.testl_i32m(mask, word.offset, word.base);
        return condition;
    }
    int32_t mask = static_cast<int32_t>(constant);
    isIndexed ? assembler.testq_i32m(mask, word.offset, word.base, word.index, word.shift) : assembler.testq_i32m(mask, word.offset, word.base);
    return condition;
}

static MacroAssembler::RelationalCondition emitFlagsOfX86Comparison(Encoder& encoder, X86Comparison comparison)
{
    if (const Operation* number = std::exchange(comparison.linkedNumberOnRight, nullptr)) {
        RELEASE_ASSERT(comparison.kind == Test::CompareIntegers && comparison.width == Width32 && comparison.right.isConstant());
        return encoder.m_relocations.withNumberAsImmediate(number->symbol(), [&](int32_t placeholder) {
            comparison.right = X86ComparedOperand::forConstant(placeholder);
            return emitFlagsOfX86Comparison(encoder, comparison);
        });
    }
    CCallHelpers& jit = encoder.m_jit;
    bool testsBits = comparison.kind == Test::TestBits;
    RELEASE_ASSERT(testsBits || comparison.kind == Test::CompareIntegers);
    RELEASE_ASSERT(!testsBits || comparison.relation == Equal || comparison.relation == NotEqual);
    RELEASE_ASSERT(comparison.width == Width32 || comparison.width == Width64);
    std::optional<MacroAssembler::RelationalCondition> condition = conditionsOfX86Comparison(comparison.relation).ofIntegers;
    RELEASE_ASSERT(condition);

    X86ComparedOperand& left = comparison.left;
    X86ComparedOperand& right = comparison.right;
    Width width = comparison.width;

    if (comparison.rightIsNumberOfBit) {
        RELEASE_ASSERT(testsBits && left.isInRegister() && right.isInRegister());
        if (width == Width32)
            jit.m_assembler.bt_ir(right.gpr(), left.gpr());
        else
            jit.m_assembler.btw_ir(right.gpr(), left.gpr());
        return comparison.relation == NotEqual ? MacroAssembler::Below : MacroAssembler::AboveOrEqual;
    }

    if (testsBits && (left.isConstant() || (left.isInRegister() && right.isInMemory())))
        std::swap(left, right);
    if (testsBits && width == Width64 && right.isConstant() && isRepresentableAs<uint32_t>(right.constant))
        width = Width32;

    if (left.isConstant())
        bringX86ComparedIntegerIntoScratch(jit, width, left, right);
    if (right.isConstant() && width == Width64 && !isRepresentableAs<int32_t>(right.constant)) {
        if (!testsBits && left.isInRegister()) {
            auto bytes = std::bit_cast<std::array<uint8_t, sizeof(uint64_t)>>(right.constant);
            uint32_t index = encoder.m_proc.addConstantData(std::span<const uint8_t>(bytes), sizeof(uint64_t), ConstantKind::Number);
            encoder.m_relocations.compare64(left.gpr(), CodeSymbol { SymbolSpace::Constant, index });
            return *condition;
        }
        bringX86ComparedIntegerIntoScratch(jit, width, right, left);
    }
    if (left.isInMemory() && right.isConstant()) {
        if (std::optional<MacroAssembler::RelationalCondition> conditionInPlace = tryEmitFlagsOfX86WordInMemory(jit, comparison.kind, left, right.constant, width, *condition))
            return *conditionInPlace;
    }
    if (left.isInMemory() && !left.isWholeWordAtBase())
        bringX86ComparedIntegerIntoScratch(jit, width, left, right);
    if (right.isInMemory() && !right.isWholeWordAtBase())
        bringX86ComparedIntegerIntoScratch(jit, width, right, left);
    if (left.isInMemory() && right.isInMemory())
        bringX86ComparedIntegerIntoScratch(jit, width, right, left);
    if (testsBits && width == Width32 && left.isInMemory() && right.isInRegister())
        bringX86ComparedIntegerIntoScratch(jit, width, left, right);

    bool is32 = width == Width32;
    MacroAssembler::TrustedImm32 immediate(static_cast<int32_t>(right.constant));
    auto compare = [&](auto leftOperand, auto rightOperand) {
        if (is32)
            jit.compareOnFlags32(leftOperand, rightOperand);
        else
            jit.compareOnFlags64(leftOperand, rightOperand);
    };

    if (testsBits) {
        auto test = [&](GPRReg value, auto mask) {
            if (is32)
                jit.testOnFlags32(value, mask);
            else
                jit.testOnFlags64(value, mask);
        };

        if (left.isInRegister()) {
            if (right.isInRegister())
                test(left.gpr(), right.gpr());
            else
                test(left.gpr(), immediate);
            return *condition;
        }

        MacroAssembler::Address address = left.address();
        if (right.isInRegister())
            jit.testOnFlags64(address, right.gpr());
        else if (immediate.m_value == -1)
            compare(address, MacroAssembler::TrustedImm32(0));
        else if (is32)
            jit.m_assembler.testl_i32m(immediate.m_value, address.offset, address.base);
        else
            jit.m_assembler.testq_i32m(immediate.m_value, address.offset, address.base);
        return *condition;
    }

    if (left.isInMemory()) {
        if (right.isInRegister())
            compare(left.address(), right.gpr());
        else
            compare(left.address(), immediate);
        return *condition;
    }

    switch (right.kind) {
    case X86ComparedOperand::InRegister:
        compare(left.gpr(), right.gpr());
        break;
    case X86ComparedOperand::InMemory:
        compare(left.gpr(), right.address());
        break;
    case X86ComparedOperand::IsConstant:
        compare(left.gpr(), immediate);
        break;
    }
    return *condition;
}

static MacroAssembler::DoubleCondition numberConditionOfX86Comparison(const X86Comparison& comparison)
{
    RELEASE_ASSERT(comparison.kind == Test::CompareNumbers);
    std::optional<MacroAssembler::DoubleCondition> condition = conditionsOfX86Comparison(comparison.relation).ofNumbers;
    RELEASE_ASSERT(condition);
    return *condition;
}

static void bringX86ComparedNumbersIntoRegisters(CCallHelpers& jit, X86Comparison& comparison)
{
    RELEASE_ASSERT(comparison.width == Width32 || comparison.width == Width64);
    bool isDouble = comparison.width == Width64;
    auto bring = [&](X86ComparedOperand& operand, const X86ComparedOperand& other) {
        if (operand.isInRegister())
            return;
        RELEASE_ASSERT(!other.mentions(scratchFPR));
        if (operand.isInMemory()) {
            if (isDouble)
                jit.loadDouble(operand.address(), scratchFPR);
            else
                jit.loadFloat(operand.address(), scratchFPR);
        } else {
            RELEASE_ASSERT(!other.mentions(scratchGPR));
            if (isDouble)
                jit.move64ToDouble(MacroAssembler::TrustedImm64(operand.constant), scratchFPR);
            else
                jit.move32ToFloat(MacroAssembler::TrustedImm32(static_cast<int32_t>(operand.constant)), scratchFPR);
        }
        operand = X86ComparedOperand::inRegister(scratchFPR);
    };
    bring(comparison.left, comparison.right);
    bring(comparison.right, comparison.left);
}

static MacroAssembler::JumpList branchOnX86ComparedNumbers(CCallHelpers& jit, X86Comparison comparison, bool whenItHolds)
{
    MacroAssembler::DoubleCondition condition = numberConditionOfX86Comparison(comparison);
    if (!whenItHolds)
        condition = MacroAssembler::invert(condition);

    bringX86ComparedNumbersIntoRegisters(jit, comparison);
    FPRReg left = comparison.left.fpr();
    FPRReg right = comparison.right.fpr();
    auto branch = [&](MacroAssembler::DoubleCondition tested) {
        return comparison.width == Width64 ? jit.branchDouble(tested, left, right) : jit.branchFloat(tested, left, right);
    };

    MacroAssembler::JumpList jumps;
    if (condition == MacroAssembler::DoubleNotEqualOrUnordered && left != right) {
        jumps.append(branch(MacroAssembler::DoubleNotEqualAndOrdered));
        jumps.append(MacroAssembler::Jump(jit.m_assembler.jp()));
        return jumps;
    }
    jumps.append(branch(condition));
    return jumps;
}

static MacroAssembler::JumpList branchOnX86Comparison(Encoder& encoder, const X86Comparison& comparison, bool whenItHolds)
{
    CCallHelpers& jit = encoder.m_jit;
    switch (comparison.kind) {
    case Test::CompareNumbers:
        return branchOnX86ComparedNumbers(jit, comparison, whenItHolds);
    case Test::CompareIntegers:
    case Test::TestBits: {
        MacroAssembler::RelationalCondition condition = emitFlagsOfX86Comparison(encoder, comparison);
        return MacroAssembler::JumpList(jit.branchOnFlags(whenItHolds ? condition : MacroAssembler::invert(condition)));
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

struct X86SelectArm {
    const Operation* value;
    Location location;
};

static X86SelectArm takeArmOfX86Select(const Encoder& encoder, const Operation* select, unsigned childIndex, LeafCursor& cursor)
{
    const Operation* value = select->child(childIndex);
    RELEASE_ASSERT(!encoder.m_liveness.isFused(value));
    return X86SelectArm { value, cursor.take() };
}

static bool canMoveArmOfX86SelectWithoutClobberingFlags(const X86SelectArm& arm)
{
    return !arm.location.isImmediate() || arm.value->hasInt();
}

static void moveArmOfX86Select(Encoder& encoder, const X86SelectArm& arm, Reg destination)
{
    CCallHelpers& jit = encoder.m_jit;
    Type type = arm.value->type();

    switch (arm.location.kind()) {
    case Location::Register:
        if (arm.location.reg() == destination)
            return;
        if (destination.isFPR())
            jit.moveDouble(arm.location.fpr(), destination.fpr());
        else if (type == Int32)
            jit.zeroExtend32ToWord(arm.location.gpr(), destination.gpr());
        else
            jit.move(arm.location.gpr(), destination.gpr());
        return;

    case Location::Spill:
    case Location::Outgoing:
    case Location::InstanceWord: {
        MacroAssembler::Address address = encoder.addressOf(arm.location);
        switch (type.kind()) {
        case Int32:
            jit.load32(address, destination.gpr());
            return;
        case Int64:
            jit.load64(address, destination.gpr());
            return;
        case Float:
            jit.loadFloat(address, destination.fpr());
            return;
        case Double:
            jit.loadDouble(address, destination.fpr());
            return;
        case Void:
        case Tuple:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    case Location::Immediate: {
        RELEASE_ASSERT(canMoveArmOfX86SelectWithoutClobberingFlags(arm));
        int64_t constant = arm.value->asInt();
        if (type == Int32 || isRepresentableAs<uint32_t>(constant)) {
            if (!tryMoveSmallConstant(jit, static_cast<int32_t>(constant), destination.gpr()))
                jit.moveWithoutClobberingFlags(MacroAssembler::TrustedImm32(static_cast<int32_t>(constant)), destination.gpr());
        } else
            jit.moveWithoutClobberingFlags(MacroAssembler::TrustedImm64(constant), destination.gpr());
        return;
    }

    case Location::None:
    case Location::SlotBytes:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void moveArmOfX86SelectAfterTest(Encoder& encoder, const X86SelectArm& arm, Reg destination)
{
    if (arm.location.isImmediate()) {
        encoder.encodeMaterialize(arm.value, destination);
        return;
    }
    moveArmOfX86Select(encoder, arm, destination);
}

static void encodeX86SelectByConditionalMove(Encoder& encoder, X86Comparison comparison, const X86SelectArm& whenItHolds, const X86SelectArm& otherwise, GPRReg destination)
{
    CCallHelpers& jit = encoder.m_jit;
    bool movesWhenTestHolds = whenItHolds.location != Location::reg(destination);
    const X86SelectArm& first = movesWhenTestHolds ? otherwise : whenItHolds;
    const X86SelectArm& second = movesWhenTestHolds ? whenItHolds : otherwise;

    auto placeArms = [&] {
        GPRReg source = second.location.isRegister() ? second.location.gpr() : scratchGPR;
        moveArmOfX86Select(encoder, second, source);
        moveArmOfX86Select(encoder, first, destination);
        return source;
    };

    if (comparison.kind == Test::CompareNumbers) {
        MacroAssembler::DoubleCondition condition = numberConditionOfX86Comparison(comparison);
        if (!movesWhenTestHolds)
            condition = MacroAssembler::invert(condition);

        bringX86ComparedNumbersIntoRegisters(jit, comparison);
        GPRReg source = placeArms();
        if (comparison.width == Width64)
            jit.moveConditionallyDouble(condition, comparison.left.fpr(), comparison.right.fpr(), source, destination);
        else
            jit.moveConditionallyFloat(condition, comparison.left.fpr(), comparison.right.fpr(), source, destination);
        return;
    }

    MacroAssembler::RelationalCondition condition = emitFlagsOfX86Comparison(encoder, comparison);
    if (!movesWhenTestHolds)
        condition = MacroAssembler::invert(condition);
    GPRReg source = placeArms();
    jit.moveConditionallyOnFlags64(condition, source, destination);
}

static void encodeX86SelectByBranching(Encoder& encoder, const X86Comparison& comparison, const X86SelectArm& whenItHolds, const X86SelectArm& otherwise, Reg destination)
{
    CCallHelpers& jit = encoder.m_jit;
    Location inDestination = Location::reg(destination);

    if (otherwise.location == inDestination) {
        MacroAssembler::JumpList done = branchOnX86Comparison(encoder, comparison, false);
        moveArmOfX86SelectAfterTest(encoder, whenItHolds, destination);
        done.link(&jit);
        return;
    }
    if (whenItHolds.location == inDestination) {
        MacroAssembler::JumpList done = branchOnX86Comparison(encoder, comparison, true);
        moveArmOfX86SelectAfterTest(encoder, otherwise, destination);
        done.link(&jit);
        return;
    }

    bool testReadsDestination = comparison.left.mentions(destination) || comparison.right.mentions(destination);
    if (!testReadsDestination && !otherwise.location.isImmediate()) {
        moveArmOfX86Select(encoder, otherwise, destination);
        MacroAssembler::JumpList done = branchOnX86Comparison(encoder, comparison, false);
        moveArmOfX86SelectAfterTest(encoder, whenItHolds, destination);
        done.link(&jit);
        return;
    }

    MacroAssembler::JumpList holds = branchOnX86Comparison(encoder, comparison, true);
    moveArmOfX86SelectAfterTest(encoder, otherwise, destination);
    MacroAssembler::Jump done = jit.jump();
    holds.link(&jit);
    moveArmOfX86SelectAfterTest(encoder, whenItHolds, destination);
    done.link(&jit);
}

MacroAssembler::JumpList Encoder::branchOn(const Test& test, bool whenItHolds)
{
    AllowMacroScratchRegisterUsage allowScratch(m_jit);
    return branchOnX86Comparison(*this, x86ComparisonOfTest(*this, test), whenItHolds);
}

static ALWAYS_INLINE std::optional<MacroAssembler::RelationalCondition> tryEmitFlagsOfX86Register(CCallHelpers& jit, bool testsBits, Opcode relation, Width width, GPRReg left, Location right, int64_t constant)
{
    bool rightIsRegister = right.isRegister();
    if (!rightIsRegister && !right.isImmediate())
        return std::nullopt;

    bool is32 = width == Width32;
    if (!rightIsRegister) {
        if (testsBits && isRepresentableAs<uint32_t>(constant))
            is32 = true;
        if (!is32 && !isRepresentableAs<int32_t>(constant))
            return std::nullopt;
    }

    RELEASE_ASSERT(!testsBits || relation == Equal || relation == NotEqual);
    RELEASE_ASSERT(width == Width32 || width == Width64);
    std::optional<MacroAssembler::RelationalCondition> condition = conditionsOfX86Comparison(relation).ofIntegers;
    RELEASE_ASSERT(condition);

    if (rightIsRegister) {
        if (testsBits)
            is32 ? jit.testOnFlags32(left, right.gpr()) : jit.testOnFlags64(left, right.gpr());
        else
            is32 ? jit.compareOnFlags32(left, right.gpr()) : jit.compareOnFlags64(left, right.gpr());
        return condition;
    }

    MacroAssembler::TrustedImm32 immediate(static_cast<int32_t>(constant));
    if (testsBits)
        is32 ? jit.testOnFlags32(left, immediate) : jit.testOnFlags64(left, immediate);
    else
        is32 ? jit.compareOnFlags32(left, immediate) : jit.compareOnFlags64(left, immediate);
    return condition;
}

static ALWAYS_INLINE std::optional<MacroAssembler::RelationalCondition> tryEmitFlagsOfX86RegisterAndMemory(Encoder& encoder, bool testsBits, Opcode relation, Width width, GPRReg left, Location right)
{
    ASSERT(right.isMemory());
    if (testsBits && width == Width32)
        return std::nullopt;

    RELEASE_ASSERT(!testsBits || relation == Equal || relation == NotEqual);
    RELEASE_ASSERT(width == Width32 || width == Width64);
    std::optional<MacroAssembler::RelationalCondition> condition = conditionsOfX86Comparison(relation).ofIntegers;
    RELEASE_ASSERT(condition);

    CCallHelpers& jit = encoder.m_jit;
    MacroAssembler::Address address = encoder.addressOf(right);
    if (testsBits)
        jit.testOnFlags64(address, left);
    else if (width == Width32)
        jit.compareOnFlags32(left, address);
    else
        jit.compareOnFlags64(left, address);
    return condition;
}

static std::optional<MacroAssembler::RelationalCondition> tryEmitFlagsOfX86TestOfRegister(CCallHelpers& jit, const Test& test)
{
    if (test.loadOnLeft || test.rightIsNumberOfBit || test.linkedNumberOnRight || !test.left.isRegister())
        return std::nullopt;
    RELEASE_ASSERT(test.kind == Test::TestBits || test.kind == Test::CompareIntegers);
    return tryEmitFlagsOfX86Register(jit, test.kind == Test::TestBits, test.relation, test.width, test.left.gpr(), test.right, test.rightConstant);
}

static std::optional<MacroAssembler::RelationalCondition> tryEmitFlagsOfX86InstanceWord(Encoder& encoder, bool testsBits, Opcode relation, Width widthOfComparison, const Operation* load, CodeSymbol symbol, Location right, int64_t constant)
{
    if (!right.isImmediate() || !load->type().isInt())
        return std::nullopt;
    Width width = widthOfComparison;
    if (testsBits && width == Width64 && isRepresentableAs<uint32_t>(constant))
        width = Width32;
    if (width == Width64 && !isRepresentableAs<int32_t>(constant))
        return std::nullopt;
    RELEASE_ASSERT(!testsBits || relation == Equal || relation == NotEqual);
    std::optional<MacroAssembler::RelationalCondition> condition = conditionsOfX86Comparison(relation).ofIntegers;
    RELEASE_ASSERT(condition);
    return encoder.m_relocations.withInstanceWordAsOperand(symbol, instanceGPR, [&](MacroAssembler::Address address) {
        AddressForm form;
        form.base = address.base;
        form.offset = address.offset;
        X86ComparedOperand word = X86ComparedOperand::readBy(load->opcode(), form);
        word.offsetIsFilledLater = true;
        return tryEmitFlagsOfX86WordInMemory(encoder.m_jit, testsBits ? Test::TestBits : Test::CompareIntegers, word, constant, width, *condition);
    });
}

static ALWAYS_INLINE std::optional<MacroAssembler::RelationalCondition> tryEmitFlagsOfX86WordAndConstant(Encoder& encoder, bool testsBits, Opcode relation, const Operation* load, const AddressForm& address, Location right, int64_t constant)
{
    ASSERT(address.kind != AddressForm::InstanceSymbol);
    if (!right.isImmediate() || !load->type().isInt())
        return std::nullopt;
    Width width = widthOf(load->type());
    if (testsBits && width == Width64 && isRepresentableAs<uint32_t>(constant))
        width = Width32;
    if (width == Width64 && !isRepresentableAs<int32_t>(constant))
        return std::nullopt;
    RELEASE_ASSERT(isLoad(load->opcode()) && !load->traps() && !encoder.factsOf(load).has(ValueFlag::TakesOverTrap));
    RELEASE_ASSERT(!testsBits || relation == Equal || relation == NotEqual);
    std::optional<MacroAssembler::RelationalCondition> condition = conditionsOfX86Comparison(relation).ofIntegers;
    RELEASE_ASSERT(condition);
    return tryEmitFlagsOfX86WordInMemory(encoder.m_jit, testsBits ? Test::TestBits : Test::CompareIntegers, X86ComparedOperand::readBy(load->opcode(), address), constant, width, *condition);
}

static ALWAYS_INLINE MacroAssembler::RelationalCondition emitFlagsOfX86WordReadByFusedLoad(Encoder& encoder, bool testsBits, Opcode relation, const Operation* load, const AddressForm& address, Location right, int64_t constant)
{
    if (address.kind == AddressForm::InstanceSymbol) {
        if (std::optional<MacroAssembler::RelationalCondition> condition = tryEmitFlagsOfX86InstanceWord(encoder, testsBits, relation, widthOf(load->type()), load, address.symbol, right, constant))
            return *condition;
    } else if (std::optional<MacroAssembler::RelationalCondition> condition = tryEmitFlagsOfX86WordAndConstant(encoder, testsBits, relation, load, address, right, constant))
        return *condition;
    AllowMacroScratchRegisterUsage allowScratch(encoder.m_jit);
    X86ComparedOperand word = x86ComparedOperandReadByFusedLoad(encoder, load, address);
    return emitFlagsOfX86Comparison(encoder, X86Comparison { testsBits ? Test::TestBits : Test::CompareIntegers, relation, widthOf(load->type()), word, x86ComparedOperandAt(encoder, right, constant), false });
}

std::optional<MacroAssembler::RelationalCondition> Encoder::tryEmitFlagsOfPredicate(const Step& step, const Operation* predicate)
{
    ASSERT(predicate == childWithoutCheck(step.operation, 0));
    if (!isFused(predicate)) {
        Location location = step.sources[0];
        if (!location.isRegister())
            return std::nullopt;
        RELEASE_ASSERT(predicate->type().isInt());
        return tryEmitFlagsOfX86Register(m_jit, true, NotEqual, widthOf(predicate->type()), location.gpr(), location, 0);
    }

    Opcode opcode = predicate->opcode();
    if (isLoad(opcode)) {
        RELEASE_ASSERT(predicate->type().isInt());
        LeafCursor cursor(step);
        AddressForm address = resolveAddress(predicate, cursor);
        return emitFlagsOfX86WordReadByFusedLoad(*this, true, NotEqual, predicate, address, Location::immediate(), -1);
    }

    bool testsBits = true;
    Opcode relation = NotEqual;
    const Operation* fused = predicate;
    if (opcode != BitAnd) {
        if (!isComparison(opcode))
            return std::nullopt;
        relation = opcode;
        const Operation* compared = inlineChildWithoutCheck(predicate, 0);
        if (compared->opcode() == BitAnd && isFused(compared)) {
            RELEASE_ASSERT((opcode == Equal || opcode == NotEqual) && inlineChildWithoutCheck(predicate, 1)->isInt(0));
            fused = compared;
        } else {
            if (compared->type().isFloat())
                return std::nullopt;
            testsBits = false;
        }
    }

    const Operation* left = inlineChildWithoutCheck(fused, 0);
    const Operation* right = inlineChildWithoutCheck(fused, 1);
    if (left->opcode() == NumberOf || right->opcode() == NumberOf)
        return std::nullopt;
    if (isFused(right)) {
        if (isFused(left) || !isLoad(right->opcode()))
            return std::nullopt;
        LeafCursor cursor(step);
        Location leftLocation = cursor.take();
        AddressForm address = resolveAddress(right, cursor);
        RELEASE_ASSERT(leftLocation);
        int64_t constant = 0;
        if (leftLocation.isImmediate()) {
            RELEASE_ASSERT(left->hasInt());
            constant = left->asInt();
        }
        return emitFlagsOfX86WordReadByFusedLoad(*this, testsBits, testsBits ? relation : flippedComparison(relation), right, address, leftLocation, constant);
    }

    if (isFused(left)) {
        if (!isLoad(left->opcode()))
            return std::nullopt;
        LeafCursor cursor(step);
        AddressForm address = resolveAddress(left, cursor);
        Location rightLocation = cursor.take();
        RELEASE_ASSERT(rightLocation);
        int64_t constant = 0;
        if (rightLocation.isImmediate()) {
            RELEASE_ASSERT(right->hasInt());
            constant = right->asInt();
        }
        return emitFlagsOfX86WordReadByFusedLoad(*this, testsBits, relation, left, address, rightLocation, constant);
    }

    Location leftLocation = step.sources[0];
    Location rightLocation = step.sources[1];
    if (!leftLocation.isRegister())
        return std::nullopt;
    if (rightLocation.isMemory())
        return tryEmitFlagsOfX86RegisterAndMemory(*this, testsBits, relation, widthOf(left->type()), leftLocation.gpr(), rightLocation);
    int64_t constant = 0;
    if (rightLocation.isImmediate()) {
        RELEASE_ASSERT(right->isConstant());
        constant = right->hasInt() ? right->asInt() : right->constantBits();
    }
    return tryEmitFlagsOfX86Register(m_jit, testsBits, relation, widthOf(left->type()), leftLocation.gpr(), rightLocation, constant);
}

MacroAssembler::Jump Encoder::jumpOnIntegers(const Test& test, bool whenItHolds)
{
    ASSERT(test.kind != Test::CompareNumbers);
    AllowMacroScratchRegisterUsage allowScratch(m_jit);
    std::optional<MacroAssembler::RelationalCondition> conditionOfRegister = tryEmitFlagsOfX86TestOfRegister(m_jit, test);
    if (!conditionOfRegister && test.loadOnLeft && test.addressOnLeft.kind == AddressForm::InstanceSymbol && !test.rightIsNumberOfBit && !test.linkedNumberOnRight)
        conditionOfRegister = tryEmitFlagsOfX86InstanceWord(*this, test.kind == Test::TestBits, test.relation, test.width, test.loadOnLeft, test.addressOnLeft.symbol, test.right, test.rightConstant);
    MacroAssembler::RelationalCondition condition = conditionOfRegister ? *conditionOfRegister : emitFlagsOfX86Comparison(*this, x86ComparisonOfTest(*this, test));
    return m_jit.branchOnFlags(whenItHolds ? condition : MacroAssembler::invert(condition));
}

void Encoder::encodeComparison(const Step& step)
{
    RELEASE_ASSERT(step.kind == StepKind::Execute && step.destination.isRegister());
    const Operation* operation = step.operation;
    ASSERT(isComparison(operation->opcode()) && operation->type() == Int32);

    AllowMacroScratchRegisterUsage allowScratch(m_jit);
    GPRReg destination = step.destination.gpr();
    LeafCursor cursor(step);
    X86Comparison comparison = takeX86Comparison(*this, operation, cursor);

    if (comparison.kind == Test::CompareNumbers) {
        MacroAssembler::DoubleCondition condition = numberConditionOfX86Comparison(comparison);
        bringX86ComparedNumbersIntoRegisters(m_jit, comparison);
        if (comparison.width == Width64)
            m_jit.compareDouble(condition, comparison.left.fpr(), comparison.right.fpr(), destination);
        else
            m_jit.compareFloat(condition, comparison.left.fpr(), comparison.right.fpr(), destination);
        return;
    }

    bool clearsDestinationFirst = !comparison.left.mentions(destination) && !comparison.right.mentions(destination);
    if (clearsDestinationFirst)
        m_jit.move(MacroAssembler::TrustedImm32(0), destination);
    MacroAssembler::RelationalCondition condition = emitFlagsOfX86Comparison(*this, comparison);
    m_jit.m_assembler.setCC_r(static_cast<X86Assembler::Condition>(condition), destination);
    if (!clearsDestinationFirst)
        m_jit.zeroExtend8To32(destination, destination);
}

void Encoder::encodeSelect(const Step& step)
{
    RELEASE_ASSERT(step.kind == StepKind::Execute && step.destination.isRegister());
    const Operation* operation = step.operation;
    ASSERT(operation->opcode() == Select);

    AllowMacroScratchRegisterUsage allowScratch(m_jit);
    LeafCursor cursor(step);
    X86Comparison comparison = x86ComparisonOfTest(*this, resolveTest(operation, cursor));
    X86SelectArm whenItHolds = takeArmOfX86Select(*this, operation, 1, cursor);
    X86SelectArm otherwise = takeArmOfX86Select(*this, operation, 2, cursor);

    Reg destination = step.destination.reg();
    if (destination.isGPR() && canMoveArmOfX86SelectWithoutClobberingFlags(whenItHolds) && canMoveArmOfX86SelectWithoutClobberingFlags(otherwise)) {
        encodeX86SelectByConditionalMove(*this, comparison, whenItHolds, otherwise, destination.gpr());
        return;
    }
    encodeX86SelectByBranching(*this, comparison, whenItHolds, otherwise, destination);
}

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT) && CPU(X86_64)
