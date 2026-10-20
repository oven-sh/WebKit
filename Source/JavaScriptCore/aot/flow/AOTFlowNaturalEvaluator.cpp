/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowValue.h"

#if ENABLE(AOT)

#include "AOTEdgeResults.h"
#include "AOTFlowObjects.h"
#include "AOTResultRules.h"
#include <algorithm>
#include <tuple>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t operationIndex = FlowNaturalEvaluator::maxOperandCount;

static constexpr uint16_t wideKeyPrimitives = Primitive::String | Primitive::Symbol;
static constexpr uint16_t literalKeyPrimitives = Primitive::LiteralString | Primitive::LiteralSymbol;

static uint16_t widenLiterals(uint16_t primitives)
{
    uint16_t widened = primitives & ~literalKeyPrimitives;
    if (primitives & Primitive::LiteralString)
        widened |= Primitive::String;
    if (primitives & Primitive::LiteralSymbol)
        widened |= Primitive::Symbol;
    return widened;
}

static uint16_t nonLiteralKeyPrimitives(const FlowValue& operand)
{
    uint16_t primitives = operand.primitives & wideKeyPrimitives;
    if (operand.primitives & ~(Primitive::AnyString | Primitive::AnySymbol))
        primitives |= Primitive::String;
    if (operand.mayBeObject())
        primitives |= wideKeyPrimitives;
    return primitives;
}

static FlowValue literalKeysOf(const FlowValue& operand)
{
    FlowValue literals;
    literals.primitives = operand.primitives & literalKeyPrimitives;
    literals.strings = operand.strings;
    literals.symbols = operand.symbols;
    return literals;
}

static BuiltinClassID singleBuiltinClassOf(const FlowObjectTable& objects, const FlowValue& value)
{
    BuiltinClassID builtinClass;
    bool hasSingleClass = value.objects.allOf([&](FlowObjectID object) {
        const FlowObject& record = objects.at(object);
        if (record.kind != FlowObjectKind::Instance || !record.builtinClass)
            return false;
        if (builtinClass && builtinClass != record.builtinClass)
            return false;
        builtinClass = record.builtinClass;
        return true;
    });
    return hasSingleClass ? builtinClass : BuiltinClassID();
}

FlowNaturalEvaluator::FlowNaturalEvaluator(const Program& program, FlowValueTable& values, TypeArena& types)
    : m_program(program)
    , m_values(values)
    , m_types(types)
{
    m_scratch.values.grow(maxOperandCount + 1);
    for (Value& value : m_scratch.values)
        value.become(Opcode::Parameter);
}

const Value& FlowNaturalEvaluator::copyWithTypesOf(const Value& operation, std::span<const FlowValue> operands)
{
    RELEASE_ASSERT(!operands.empty() && operands.size() <= maxOperandCount);
    RELEASE_ASSERT(operands.size() == operation.numberOfOperands());

    Value& copy = m_scratch.values[operationIndex];
    copy = operation;
    for (uint32_t index = 0; index < operands.size(); ++index) {
        m_scratch.values[index].type = typeFor(operands[index]);
        copy.setOperand(index, ValueID(index));
    }
    return copy;
}

static bool hasEmptyValue(std::span<const FlowValue> operands)
{
    return std::ranges::any_of(operands, &FlowValue::isEmpty);
}

// Only a range gets narrower: a type says less than a value about strings, symbols and objects.
FlowValue FlowNaturalEvaluator::narrowOperand(const Value& test, uint32_t operandIndex, bool isOnTakenEdge, std::span<const FlowValue> operands)
{
    RELEASE_ASSERT(provesOf(test.opcode) == Proof::IntegerBound);
    if (hasEmptyValue(operands))
        return { };

    const FlowValue& operand = operands[operandIndex];
    if (operand.primitives != Primitive::Integer || operand.mayBeObject())
        return operand;
    const Value& copy = copyWithTypesOf(test, operands);
    return m_values.valueOfType(narrowOperandByTest(TypingContext { m_program, m_scratch, m_types }, copy, operandIndex, isOnTakenEdge ? EdgeSide::Taken : EdgeSide::NotTaken));
}

FlowValue FlowNaturalEvaluator::evaluate(const Value& operation, std::span<const FlowValue> operands)
{
    RELEASE_ASSERT(resultRuleOf(operation.opcode) == ResultRule::Natural);
    if (hasEmptyValue(operands))
        return { };

    const Value& copy = copyWithTypesOf(operation, operands);
    Type result = computeResultType(TypingContext { m_program, m_scratch, m_types }, copy);
    RELEASE_ASSERT(result.hasOnlyPrimitives(Primitive::Defined));

    FlowValue value = m_values.valueOfType(result);
    if (operation.opcode == Opcode::ToPropertyKey) {
        value.primitives &= ~wideKeyPrimitives | nonLiteralKeyPrimitives(operands[0]);
        m_values.join(value, literalKeysOf(operands[0]), Subject { });
    }
    return value;
}

TypeID FlowNaturalEvaluator::typeFor(const FlowValue& value)
{
    ASSERT(!!(value.primitives & Primitive::Integer) == !!value.integers);
    uint16_t primitives = widenLiterals(value.primitives);
    BuiltinClassID builtinClass = singleBuiltinClassOf(m_values.objects(), value);
    bool mayBeAnyObject = value.mayBeObject() && !builtinClass;
    std::tuple<uint16_t, uint32_t, bool, unsigned> key { primitives, value.integers.index(), mayBeAnyObject, builtinClass.index() };
    return m_typesByValue.ensure(key, [&] {
        Type type = builtinClass ? Type::builtinClass(builtinClass) : Type::never();
        type.primitives = primitives & ~Primitive::Integer;
        if (value.integers) {
            type.primitives |= Primitive::Integer;
            type.integers = m_values.rangeOf(value.integers);
        }
        type.anyObject = mayBeAnyObject;
        type.anyFunction = mayBeAnyObject;
        return m_types.intern(type);
    }).iterator->value;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
