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

// What an operand may be once it has been converted to a primitive. A symbol throws.
struct ConvertedOperand {
    bool mayBeNumberLike { false };
    bool mayBeString { false };
    bool mayBeBigInt { false };
    // Among what is like a number. `undefined` becomes NaN, which counts as a number without a fraction, null and the booleans become 0 and 1.
    bool mayBeNumberWithFraction { false };

    bool mayBeAnything() const { return mayBeNumberLike || mayBeString || mayBeBigInt; }
    bool mayBecomeNumber() const { return mayBeNumberLike || mayBeString; }
};

} // anonymous namespace

static std::optional<ArithmeticForm> formOf(Opcode opcode)
{
    switch (opcode) {
    case Opcode::Add:
        return ArithmeticForm::Addition;
    case Opcode::Sub:
    case Opcode::Mul:
    case Opcode::Div:
    case Opcode::Mod:
    case Opcode::Pow:
    case Opcode::BitAnd:
    case Opcode::BitOr:
    case Opcode::BitXor:
    case Opcode::LShift:
    case Opcode::RShift:
        return ArithmeticForm::BinaryNumeric;
    case Opcode::Negate:
    case Opcode::ToNumeric:
    case Opcode::Increment:
    case Opcode::Decrement:
    case Opcode::BitNot:
        return ArithmeticForm::UnaryNumeric;
    default:
        return std::nullopt;
    }
}

bool answersByKindsOfOperands(Opcode opcode)
{
    return formOf(opcode) || opcode == Opcode::ToPropertyKey;
}

static ValueSetID kinds(ValueFlowRuleContext& context, uint16_t primitives)
{
    Vector<ValueFlowObjectID, 16> members;
    for (uint16_t rest = primitives; rest; rest &= rest - 1)
        members.append(ValueFlowUniverse::primitive(static_cast<PrimitiveSet>(rest & -rest)));
    return context.maker().of(members.span());
}

// Code of the operand converts an object, and it may answer any primitive.
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

// As the types have it: see fractionFreeOperators.
bool makesNoFraction(Opcode opcode)
{
    switch (opcode) {
    case Opcode::Add:
    case Opcode::Sub:
    case Opcode::Mul:
    case Opcode::Mod:
    case Opcode::Negate:
    case Opcode::Increment:
    case Opcode::Decrement:
    case Opcode::ToNumeric:
        return true;
    default:
        return false;
    }
}

// An operator on bits makes an integer of 32 bits of whatever number it is given, and answers one.
static bool answersInt32(Opcode opcode)
{
    switch (opcode) {
    case Opcode::BitAnd:
    case Opcode::BitOr:
    case Opcode::BitXor:
    case Opcode::LShift:
    case Opcode::RShift:
    case Opcode::BitNot:
        return true;
    default:
        return false;
    }
}

// The result has the kinds that the kinds of the operands can give: a sum of numbers is a number. The mask of the result keeps what the type of the operation admits.
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

    ArithmeticForm form = *formOf(statement.opcode);
    ConvertedOperand left = convert(context, operands[0]);
    ConvertedOperand right = form == ArithmeticForm::UnaryNumeric ? left : convert(context, operands[1]);

    // A sum, a difference and a product of numbers without a fraction have none. A sum takes a string as it is. Every other operator makes a number of it, and the text may spell a fraction.
    bool takesFraction = left.mayBeNumberWithFraction || right.mayBeNumberWithFraction || (form != ArithmeticForm::Addition && (left.mayBeString || right.mayBeString));
    uint16_t number = answersInt32(statement.opcode) ? Primitive::Integer : makesNoFraction(statement.opcode) && !takesFraction ? Primitive::NumberWithoutFraction : Primitive::Number;

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

// A string and a symbol are the key themselves, so a key that the analysis knows stays known. Where nothing arrives there is no key.
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
