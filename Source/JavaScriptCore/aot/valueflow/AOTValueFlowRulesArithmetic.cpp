/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowRules.h"

#if ENABLE(AOT)

#include "AOTSchemaOperations.h"
#include "AOTValueFlowBuiltinTables.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

enum class ArithmeticForm : uint8_t {
    Addition,
    BinaryNumeric,
    UnaryNumeric,
};

struct OperatorOnKinds {
    ArithmeticForm form;
    bool makesNoFraction { false };
    bool answersInt32 { false };
};

struct ConvertedOperand {
    bool mayBeNumberLike { false };
    bool mayBeString { false };
    bool mayBeBigInt { false };
    bool mayBeNumberWithFraction { false };

    bool mayBeAnything() const { return mayBeNumberLike || mayBeString || mayBeBigInt; }
    bool mayBecomeNumber() const { return mayBeNumberLike || mayBeString; }
};

} // anonymous namespace

static std::optional<OperatorOnKinds> operatorOnKinds(Opcode opcode)
{
    switch (opcode) {
    case Opcode::Add:
        return OperatorOnKinds { ArithmeticForm::Addition, true };
    case Opcode::Sub:
    case Opcode::Mul:
    case Opcode::Mod:
        return OperatorOnKinds { ArithmeticForm::BinaryNumeric, true };
    case Opcode::Div:
    case Opcode::Pow:
        return OperatorOnKinds { ArithmeticForm::BinaryNumeric };
    case Opcode::BitAnd:
    case Opcode::BitOr:
    case Opcode::BitXor:
    case Opcode::LShift:
    case Opcode::RShift:
        return OperatorOnKinds { ArithmeticForm::BinaryNumeric, false, true };
    case Opcode::Negate:
    case Opcode::ToNumeric:
    case Opcode::Increment:
    case Opcode::Decrement:
        return OperatorOnKinds { ArithmeticForm::UnaryNumeric, true };
    case Opcode::BitNot:
        return OperatorOnKinds { ArithmeticForm::UnaryNumeric, false, true };
    default:
        return std::nullopt;
    }
}

bool answersByKindsOfOperands(Opcode opcode) { return operatorOnKinds(opcode) || opcode == Opcode::ToPropertyKey; }

bool makesNoFraction(Opcode opcode)
{
    std::optional<OperatorOnKinds> traits = operatorOnKinds(opcode);
    return traits && traits->makesNoFraction;
}

static ValueSetID kinds(ValueFlowRuleContext& context, uint16_t primitives)
{
    Vector<ValueFlowObjectID, 16> members;
    for (uint16_t rest = primitives; rest; rest &= rest - 1)
        members.append(ValueFlowUniverse::primitive(static_cast<PrimitiveSet>(rest & -rest)));
    return context.maker().of(members.span());
}

static ConvertedOperand convert(ValueFlowRuleContext& context, const ValueFlowOperand& operand)
{
    const ValueFlowUniverse& universe = context.universe();
    ClimbingSet value = context.valueOf(operand);
    bool isConvertedByCode = context.meet(value, universe.objects());
    return {
        isConvertedByCode || context.meet(value, kinds(context, Primitive::NumberLike)),
        isConvertedByCode || context.meet(value, kinds(context, Primitive::String)) || context.meet(value, universe.keys()),
        isConvertedByCode || context.meet(value, kinds(context, Primitive::BigInt)),
        isConvertedByCode || context.meet(value, kinds(context, Primitive::FractionalNumber)),
    };
}

void runArithmetic(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    std::span<const ValueFlowOperand> operands = context.operandsOf(statement);
    for (uint32_t index = 0; index < operands.size(); ++index) {
        if (ValueFlowBehaviorID behavior = context.universe().builtinTables().behaviorOf(operandInfoOf(statement.opcode, index)))
            context.connect(operands[index], context.hub(behavior));
    }
    if (statement.thrown)
        context.add(statement.thrown, unknownValue(context, ValueFlowUnknownSource::ThrownByRunTime));
    if (!statement.result)
        return;

    OperatorOnKinds traits = *operatorOnKinds(statement.opcode);
    ArithmeticForm form = traits.form;
    ConvertedOperand left = convert(context, operands[0]);
    ConvertedOperand right = form == ArithmeticForm::UnaryNumeric ? left : convert(context, operands[1]);

    bool takesFraction = left.mayBeNumberWithFraction || right.mayBeNumberWithFraction || (form != ArithmeticForm::Addition && (left.mayBeString || right.mayBeString));
    uint16_t number = traits.answersInt32 ? Primitive::Integer : traits.makesNoFraction && !takesFraction ? Primitive::NumberWithoutFraction : Primitive::Number;

    uint16_t result = 0;
    if (form == ArithmeticForm::Addition) {
        if (left.mayBeNumberLike && right.mayBeNumberLike)
            result |= number;
        if ((left.mayBeString && right.mayBeAnything()) || (right.mayBeString && left.mayBeAnything()))
            result |= Primitive::String;
    } else if (left.mayBecomeNumber() && right.mayBecomeNumber())
        result |= number;
    if (left.mayBeBigInt && right.mayBeBigInt)
        result |= Primitive::BigInt;
    if (result)
        context.add(statement.result, kinds(context, result));
}

void runToPropertyKey(const ValueFlowStatement& statement, ValueFlowRuleContext& context)
{
    const ValueFlowUniverse& universe = context.universe();
    const ValueFlowOperand& operand = context.operandsOf(statement)[0];
    if (ValueFlowBehaviorID behavior = universe.builtinTables().behaviorOf(operandInfoOf(statement.opcode, 0)))
        context.connect(operand, context.hub(behavior));
    if (statement.thrown)
        context.add(statement.thrown, unknownValue(context, ValueFlowUnknownSource::ThrownByRunTime));
    if (!statement.result)
        return;

    ClimbingSet value = context.valueOf(operand);
    context.add(statement.result, context.restrict(value, context.maker().unite(universe.strings(), universe.symbols())));
    if (context.meet(value, kinds(context, Primitive::NumberLike | Primitive::BigInt)))
        context.add(statement.result, kinds(context, Primitive::String));
    if (context.meet(value, universe.objects()))
        context.add(statement.result, kinds(context, Primitive::String | Primitive::Symbol));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
