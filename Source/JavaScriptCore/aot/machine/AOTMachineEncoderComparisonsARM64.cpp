/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTMachineEncoder.h"

#if ENABLE(AOT) && CPU(ARM64)

#include "AllowMacroScratchRegisterUsage.h"

namespace JSC { namespace AOT { namespace Machine {

static_assert(static_cast<int>(MacroAssembler::Zero) == static_cast<int>(MacroAssembler::Equal));
static_assert(static_cast<int>(MacroAssembler::NonZero) == static_cast<int>(MacroAssembler::NotEqual));

struct ConditionsOfComparison {
    std::optional<MacroAssembler::RelationalCondition> ofIntegers;
    std::optional<MacroAssembler::DoubleCondition> ofNumbers;
};

static ConditionsOfComparison conditionsOfComparison(Opcode comparison)
{
    switch (comparison) {
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

static MacroAssembler::RelationalCondition relationalConditionOfTest(const Test& test)
{
    std::optional<MacroAssembler::RelationalCondition> condition = conditionsOfComparison(test.relation).ofIntegers;
    RELEASE_ASSERT(test.kind != Test::CompareNumbers && condition);
    return *condition;
}

static MacroAssembler::DoubleCondition doubleConditionOfTest(const Test& test)
{
    std::optional<MacroAssembler::DoubleCondition> condition = conditionsOfComparison(test.relation).ofNumbers;
    RELEASE_ASSERT(test.kind == Test::CompareNumbers && condition);
    return *condition;
}

static MacroAssembler::ResultCondition resultConditionOfTest(const Test& test)
{
    RELEASE_ASSERT(test.kind == Test::TestBits && (test.relation == Equal || test.relation == NotEqual));
    return test.relation == Equal ? MacroAssembler::Zero : MacroAssembler::NonZero;
}

static GPRReg gprOfComparisonOperand(Location location)
{
    RELEASE_ASSERT(location.isRegister() && location.reg().isGPR());
    return location.gpr();
}

static FPRReg fprOfComparisonOperand(Location location)
{
    RELEASE_ASSERT(location.isRegister() && location.reg().isFPR());
    return location.fpr();
}

static bool constantOfTestFitsInt32(const Test& test)
{
    return test.rightConstant == static_cast<int32_t>(test.rightConstant);
}

struct ComparedNumbers {
    bool comparesWithZero() const { return right == InvalidFPRReg; }

    FPRReg left;
    FPRReg right;
    bool areFloats;
};

static ComparedNumbers comparedNumbersOf(const Test& test)
{
    ASSERT(test.kind == Test::CompareNumbers);
    bool areFloats = test.width == Width32;
    FPRReg left = fprOfComparisonOperand(test.left);
    if (!test.right.isImmediate())
        return { left, fprOfComparisonOperand(test.right), areFloats };

    bool isZero = areFloats ? !std::bit_cast<float>(static_cast<int32_t>(test.rightConstant)) : !std::bit_cast<double>(test.rightConstant);
    RELEASE_ASSERT(isZero);
    return { left, InvalidFPRReg, areFloats };
}

template<int width>
static void compareWithUInt12(CCallHelpers& jit, GPRReg left, int64_t constant)
{
    uint64_t magnitude = constant < 0 ? uint64_t { 0 } - static_cast<uint64_t>(constant) : static_cast<uint64_t>(constant);
    int shift = magnitude & ~uint64_t { 0xfff } ? 12 : 0;
    RELEASE_ASSERT(!(magnitude & ~(uint64_t { 0xfff } << shift)));
    UInt12 immediate(static_cast<int>(magnitude >> shift));
    if (constant < 0)
        jit.m_assembler.cmn<width>(left, immediate, shift);
    else
        jit.m_assembler.cmp<width>(left, immediate, shift);
}

static void compareWithExtendedWord(CCallHelpers& jit, const Test& test)
{
    ASSERT(test.kind == Test::CompareIntegers && test.width == Width64 && test.extensionOfRight != IndexExtension::None);
    ARM64Assembler::ExtendType type = test.extensionOfRight == IndexExtension::ZeroExtend32 ? ARM64Assembler::UXTW : ARM64Assembler::SXTW;
    jit.m_assembler.cmp<64>(gprOfComparisonOperand(test.left), gprOfComparisonOperand(test.right), type, 0);
}

static std::optional<ConstantComparison> constantComparisonOfTest(const Test& test)
{
    if (test.kind != Test::CompareIntegers || !test.right.isImmediate())
        return std::nullopt;
    std::optional<ConstantComparison> found = findConstantComparison(test.relation, test.width, test.rightConstant);
    RELEASE_ASSERT(found);
    return found;
}

static ARM64Assembler::Condition setFlagsForConstantComparison(CCallHelpers& jit, const Test& test, const ConstantComparison& comparison)
{
    bool is32 = test.width == Width32;
    GPRReg left = gprOfComparisonOperand(test.left);
    if (comparison.kind != ConstantComparison::HighBits) {
        if (is32)
            compareWithUInt12<32>(jit, left, comparison.constant);
        else
            compareWithUInt12<64>(jit, left, comparison.constant);
        if (comparison.kind == ConstantComparison::Overflow)
            return comparison.holdsOnOverflow ? ARM64Assembler::ConditionVS : ARM64Assembler::ConditionVC;
        return static_cast<ARM64Assembler::Condition>(*conditionsOfComparison(comparison.relation).ofIntegers);
    }

    bool isSigned = comparison.relation == LessThan || comparison.relation == GreaterEqual;
    ARM64Assembler::ShiftType shift = isSigned ? ARM64Assembler::ASR : ARM64Assembler::LSR;
    if (is32)
        jit.m_assembler.cmp<32>(ARM64Registers::zr, left, shift, comparison.shift);
    else
        jit.m_assembler.cmp<64>(ARM64Registers::zr, left, shift, comparison.shift);
    switch (comparison.relation) {
    case LessThan:
        return ARM64Assembler::ConditionGE;
    case GreaterEqual:
        return ARM64Assembler::ConditionLT;
    case Below:
        return ARM64Assembler::ConditionEQ;
    case AboveEqual:
        return ARM64Assembler::ConditionNE;
    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static MacroAssembler::Jump jumpOnFlags(CCallHelpers& jit, ARM64Assembler::Condition condition)
{
    jit.m_assembler.b_cond(condition);
    AssemblerLabel label = jit.m_assembler.labelIgnoringWatchpoints();
    jit.m_assembler.nop();
    return MacroAssembler::Jump(label, ARM64Assembler::JumpCondition, condition);
}

static MacroAssembler::Jump branchOnComparedIntegers(CCallHelpers& jit, MacroAssembler::RelationalCondition condition, const Test& test)
{
    bool is32 = test.width == Width32;
    GPRReg left = gprOfComparisonOperand(test.left);
    if (!test.right.isImmediate()) {
        GPRReg right = gprOfComparisonOperand(test.right);
        return is32 ? jit.branch32(condition, left, right) : jit.branch64(condition, left, right);
    }

    RELEASE_ASSERT(left != scratchGPR);
    if (is32 || constantOfTestFitsInt32(test)) {
        MacroAssembler::TrustedImm32 right(static_cast<int32_t>(test.rightConstant));
        return is32 ? jit.branch32(condition, left, right) : jit.branch64(condition, left, right);
    }
    return jit.branch64(condition, left, MacroAssembler::TrustedImm64(test.rightConstant));
}

static MacroAssembler::Jump branchOnTestedBits(CCallHelpers& jit, MacroAssembler::ResultCondition condition, const Test& test)
{
    bool is32 = test.width == Width32;
    GPRReg left = gprOfComparisonOperand(test.left);
    if (!test.right.isImmediate()) {
        GPRReg right = gprOfComparisonOperand(test.right);
        return is32 ? jit.branchTest32(condition, left, right) : jit.branchTest64(condition, left, right);
    }

    RELEASE_ASSERT(left != scratchGPR);
    if (is32)
        return jit.branchTest32(condition, left, MacroAssembler::TrustedImm32(static_cast<int32_t>(test.rightConstant)));
    return jit.branchTest64(condition, left, MacroAssembler::TrustedImm64(test.rightConstant));
}

static MacroAssembler::Jump branchOnComparedNumbers(CCallHelpers& jit, MacroAssembler::DoubleCondition condition, const Test& test)
{
    ComparedNumbers numbers = comparedNumbersOf(test);
    if (numbers.comparesWithZero())
        return numbers.areFloats ? jit.branchFloatWithZero(condition, numbers.left) : jit.branchDoubleWithZero(condition, numbers.left);
    return numbers.areFloats ? jit.branchFloat(condition, numbers.left, numbers.right) : jit.branchDouble(condition, numbers.left, numbers.right);
}

static MacroAssembler::RelationalCondition setFlagsForTestOfIntegers(CCallHelpers& jit, const Test& test)
{
    std::optional<ConstantComparison> comparison = constantComparisonOfTest(test);
    if (comparison && (comparison->kind != ConstantComparison::Immediate || comparison->relation != test.relation))
        return static_cast<MacroAssembler::RelationalCondition>(setFlagsForConstantComparison(jit, test, *comparison));

    MacroAssembler::RelationalCondition condition = relationalConditionOfTest(test);
    if (test.extensionOfRight != IndexExtension::None) {
        compareWithExtendedWord(jit, test);
        return condition;
    }
    bool asksForEquality = condition == MacroAssembler::Equal || condition == MacroAssembler::NotEqual;
    bool testsBits = test.kind == Test::TestBits;
    bool is32 = test.width == Width32;
    RELEASE_ASSERT(!testsBits || asksForEquality);

    GPRReg left = gprOfComparisonOperand(test.left);
    GPRReg right = InvalidGPRReg;
    if (!test.right.isImmediate())
        right = gprOfComparisonOperand(test.right);
    else if (!testsBits && asksForEquality && !test.rightConstant) {
        testsBits = true;
        right = left;
    } else if (!is32 && !constantOfTestFitsInt32(test)) {
        RELEASE_ASSERT(left != scratchGPR);
        right = jit.scratchRegister();
        jit.move(MacroAssembler::TrustedImm64(test.rightConstant), right);
    }

    if (right != InvalidGPRReg) {
        if (testsBits && is32)
            jit.testOnFlags32(left, right);
        else if (testsBits)
            jit.testOnFlags64(left, right);
        else if (is32)
            jit.compareOnFlags32(left, right);
        else
            jit.compareOnFlags64(left, right);
        return condition;
    }

    RELEASE_ASSERT(left != scratchGPR);
    MacroAssembler::TrustedImm32 immediate(static_cast<int32_t>(test.rightConstant));
    if (testsBits && is32)
        jit.testOnFlags32(left, immediate);
    else if (testsBits)
        jit.testOnFlags64(left, immediate);
    else if (is32)
        jit.compareOnFlags32(left, immediate);
    else
        jit.compareOnFlags64(left, immediate);
    return condition;
}

static void materializeComparedNumbers(CCallHelpers& jit, const Test& test, GPRReg destination)
{
    MacroAssembler::DoubleCondition condition = doubleConditionOfTest(test);
    ComparedNumbers numbers = comparedNumbersOf(test);
    if (numbers.comparesWithZero() && numbers.areFloats)
        jit.compareFloatWithZero(condition, numbers.left, destination);
    else if (numbers.comparesWithZero())
        jit.compareDoubleWithZero(condition, numbers.left, destination);
    else if (numbers.areFloats)
        jit.compareFloat(condition, numbers.left, numbers.right, destination);
    else
        jit.compareDouble(condition, numbers.left, numbers.right, destination);
}

static void selectIntegerOnTest(CCallHelpers& jit, const Test& test, GPRReg whenItHolds, GPRReg otherwise, GPRReg destination)
{
    if (test.kind != Test::CompareNumbers) {
        MacroAssembler::RelationalCondition condition = setFlagsForTestOfIntegers(jit, test);
        jit.moveConditionallyOnFlags64(condition, whenItHolds, otherwise, destination);
        return;
    }

    MacroAssembler::DoubleCondition condition = doubleConditionOfTest(test);
    ComparedNumbers numbers = comparedNumbersOf(test);
    if (numbers.comparesWithZero() && numbers.areFloats)
        jit.moveConditionallyFloatWithZero(condition, numbers.left, whenItHolds, otherwise, destination);
    else if (numbers.comparesWithZero())
        jit.moveConditionallyDoubleWithZero(condition, numbers.left, whenItHolds, otherwise, destination);
    else if (numbers.areFloats)
        jit.moveConditionallyFloat(condition, numbers.left, numbers.right, whenItHolds, otherwise, destination);
    else
        jit.moveConditionallyDouble(condition, numbers.left, numbers.right, whenItHolds, otherwise, destination);
}

static void selectNumberOnTest(CCallHelpers& jit, const Test& test, FPRReg whenItHolds, FPRReg otherwise, FPRReg destination)
{
    if (test.kind != Test::CompareNumbers) {
        MacroAssembler::RelationalCondition condition = setFlagsForTestOfIntegers(jit, test);
        jit.m_assembler.fcsel<64>(destination, whenItHolds, otherwise, static_cast<MacroAssembler::Condition>(condition));
        return;
    }

    MacroAssembler::DoubleCondition condition = doubleConditionOfTest(test);
    ComparedNumbers numbers = comparedNumbersOf(test);
    if (numbers.comparesWithZero() && numbers.areFloats)
        jit.moveDoubleConditionallyFloatWithZero(condition, numbers.left, whenItHolds, otherwise, destination);
    else if (numbers.comparesWithZero())
        jit.moveDoubleConditionallyDoubleWithZero(condition, numbers.left, whenItHolds, otherwise, destination);
    else if (numbers.areFloats)
        jit.moveDoubleConditionallyFloat(condition, numbers.left, numbers.right, whenItHolds, otherwise, destination);
    else
        jit.moveDoubleConditionallyDouble(condition, numbers.left, numbers.right, whenItHolds, otherwise, destination);
}

static Test testOfUnfusedComparison(const Encoder& encoder, const Step& step)
{
    const Operation* comparison = step.operation;
    const Operation* left = comparison->child(0);
    const Operation* right = comparison->child(1);
    for (const Operation* operand : { left, right })
        RELEASE_ASSERT(!encoder.m_liveness.isFused(operand) || operand->opcode() == Trunc || namesWord(operand));

    LeafCursor leaves(step);
    Location leftLocation = leaves.take();
    Location rightLocation = leaves.take();

    int64_t rightConstant = 0;
    if (rightLocation.isImmediate()) {
        RELEASE_ASSERT(right->isConstant());
        rightConstant = right->hasInt() ? right->asInt() : right->constantBits();
    }
    Test::Kind kind = left->type().isFloat() ? Test::CompareNumbers : Test::CompareIntegers;
    return Test { kind, comparison->opcode(), widthOf(left->type()), leftLocation, rightLocation, rightConstant };
}

MacroAssembler::JumpList Encoder::branchOn(const Test& test, bool whenItHolds)
{
    AllowMacroScratchRegisterUsage allowScratch(m_jit);

    MacroAssembler::JumpList jumps;
    switch (test.kind) {
    case Test::CompareIntegers: {
        std::optional<ConstantComparison> comparison = constantComparisonOfTest(test);
        if (comparison && (comparison->kind != ConstantComparison::Immediate || comparison->relation != test.relation)) {
            ARM64Assembler::Condition condition = setFlagsForConstantComparison(m_jit, test, *comparison);
            jumps.append(jumpOnFlags(m_jit, whenItHolds ? condition : ARM64Assembler::invert(condition)));
            return jumps;
        }
        MacroAssembler::RelationalCondition condition = relationalConditionOfTest(test);
        if (test.extensionOfRight != IndexExtension::None) {
            compareWithExtendedWord(m_jit, test);
            jumps.append(jumpOnFlags(m_jit, static_cast<ARM64Assembler::Condition>(whenItHolds ? condition : MacroAssembler::invert(condition))));
            return jumps;
        }
        jumps.append(branchOnComparedIntegers(m_jit, whenItHolds ? condition : MacroAssembler::invert(condition), test));
        return jumps;
    }

    case Test::CompareNumbers: {
        MacroAssembler::DoubleCondition condition = doubleConditionOfTest(test);
        jumps.append(branchOnComparedNumbers(m_jit, whenItHolds ? condition : MacroAssembler::invert(condition), test));
        return jumps;
    }

    case Test::TestBits: {
        MacroAssembler::ResultCondition condition = resultConditionOfTest(test);
        jumps.append(branchOnTestedBits(m_jit, whenItHolds ? condition : MacroAssembler::invert(condition), test));
        return jumps;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void Encoder::encodeComparison(const Step& step)
{
    ASSERT(step.kind == StepKind::Execute);
    ASSERT(isComparison(step.operation->opcode()) && step.operation->type() == Int32);

    Test test = testOfUnfusedComparison(*this, step);
    GPRReg destination = gprOfComparisonOperand(step.destination);

    AllowMacroScratchRegisterUsage allowScratch(m_jit);
    if (test.kind == Test::CompareNumbers) {
        materializeComparedNumbers(m_jit, test, destination);
        return;
    }
    MacroAssembler::RelationalCondition condition = setFlagsForTestOfIntegers(m_jit, test);
    m_jit.setOnFlags(condition, destination);
}

void Encoder::encodeSelect(const Step& step)
{
    ASSERT(step.kind == StepKind::Execute);
    const Operation* select = step.operation;
    ASSERT(select->opcode() == Select);
    for (unsigned index : { 1u, 2u })
        RELEASE_ASSERT(!m_liveness.isFused(select->child(index)) || select->child(index)->opcode() == Trunc || namesWord(select->child(index)));

    LeafCursor leaves(step);
    Test test = resolveTest(select, leaves);
    Location whenNotZero = leaves.take();
    Location whenZero = leaves.take();

    AllowMacroScratchRegisterUsage allowScratch(m_jit);
    switch (select->type().kind()) {
    case Int64: {
        bool holdsRarely = isEndOfExactIntegers(select->child(1));
        GPRReg destination = gprOfComparisonOperand(step.destination);
        GPRReg rare = gprOfComparisonOperand(holdsRarely ? whenNotZero : whenZero);
        if (test.kind != Test::CompareNumbers && holdsRarely != isEndOfExactIntegers(select->child(2)) && rare != destination && placeOf(m_block).loop != noLoop) {
            MacroAssembler::RelationalCondition condition = setFlagsForTestOfIntegers(m_jit, test);
            MacroAssembler::Jump isRare = m_jit.branchOnFlags(holdsRarely ? condition : MacroAssembler::invert(condition));
            m_jit.move(gprOfComparisonOperand(holdsRarely ? whenZero : whenNotZero), destination);
            auto moveRareArm = [rare, destination, isRare, done = m_jit.label()](CCallHelpers& jit) {
                isRare.link(&jit);
                jit.move(rare, destination);
                jit.jump().linkTo(done, &jit);
            };
            addLatePath(m_arena.create<LatePathOf<decltype(moveRareArm)>>(moveRareArm));
            return;
        }
        [[fallthrough]];
    }
    case Int32:
        selectIntegerOnTest(m_jit, test, gprOfComparisonOperand(whenNotZero), gprOfComparisonOperand(whenZero), gprOfComparisonOperand(step.destination));
        return;
    case Float:
    case Double:
        selectNumberOnTest(m_jit, test, fprOfComparisonOperand(whenNotZero), fprOfComparisonOperand(whenZero), fprOfComparisonOperand(step.destination));
        return;
    case Vector128:
    case Void:
    case Tuple:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT) && CPU(ARM64)
