/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTKnownValue.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTStringArena.h"
#include "AOTStringTable.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include "MathCommon.h"
#include "PureNaN.h"
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet primitiveConstantOpcodes { Opcode::Undefined, Opcode::Null, Opcode::Boolean, Opcode::Integer, Opcode::Double, Opcode::String };

static KnownValue toKnownValue(KnownBoolean value)
{
    switch (value) {
    case KnownBoolean::Unknown:
        return { };
    case KnownBoolean::False:
        return KnownValue::boolean(false);
    case KnownBoolean::True:
        return KnownValue::boolean(true);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

KnownValue KnownValue::number(double value)
{
    if (isTrackedInteger(value))
        return { Kind::Integer, static_cast<uint64_t>(static_cast<int64_t>(value)) };
    return { Kind::Double, std::bit_cast<uint64_t>(purifyNaN(value)) };
}

KnownBoolean KnownValue::toBoolean(const Program& program) const
{
    switch (m_kind) {
    case Kind::Unknown:
        return KnownBoolean::Unknown;
    case Kind::Undefined:
    case Kind::Null:
        return KnownBoolean::False;
    case Kind::Boolean:
        return knownBoolean(asBoolean());
    case Kind::Integer:
        return knownBoolean(!!asInteger());
    case Kind::Double:
        return knownBoolean(asNumber() && !std::isnan(asNumber()));
    case Kind::String:
        return knownBoolean(!program.string(asString()).empty());
    case Kind::Object:
    case Kind::Function:
        return KnownBoolean::True;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static KnownValue evaluateConstant(const Value& value)
{
    switch (value.opcode) {
    case Opcode::Undefined:
        return KnownValue::undefined();
    case Opcode::Null:
        return KnownValue::null();
    case Opcode::Boolean:
        return KnownValue::boolean(value.boolean());
    case Opcode::Integer:
        return KnownValue::integer(value.integer());
    case Opcode::Double:
        return KnownValue::number(value.number());
    case Opcode::String:
        return KnownValue::string(value.string());
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static KnownValue knownValueOfAllocation(const Program& program, const Value& definition)
{
    if (definition.opcode == Opcode::NewClosure || definition.opcode == Opcode::NewClass)
        return KnownValue::function();
    if (resultRuleOf(definition.opcode) != ResultRule::Allocation)
        return { };
    if (!program.at(definition.type).isJavaScriptValue())
        return { };
    return KnownValue::object();
}

static KnownValue singlePrimitiveOf(const Type& type)
{
    switch (type.primitives) {
    case Primitive::Undefined:
        return KnownValue::undefined();
    case Primitive::Null:
        return KnownValue::null();
    case Primitive::False:
        return KnownValue::boolean(false);
    case Primitive::True:
        return KnownValue::boolean(true);
    case Primitive::Integer:
        if (type.integers.minimum != type.integers.maximum)
            return { };
        return KnownValue::integer(type.integers.minimum);
    case Primitive::LiteralString:
        if (type.stringLiterals.size() != 1)
            return { };
        return KnownValue::string(type.stringLiterals[0]);
    default:
        return { };
    }
}

static bool hasOnlyCallableObjects(const Type& type)
{
    return !type.anyObject && !type.layouts && type.classes.isEmpty() && type.builtins.isEmpty();
}

KnownValue knownValueOf(const DataNode& node)
{
    switch (node.kind()) {
    case DataNode::Kind::Undefined:
        return KnownValue::undefined();
    case DataNode::Kind::Null:
        return KnownValue::null();
    case DataNode::Kind::Boolean:
        return KnownValue::boolean(node.as<DataNode::Boolean>().value);
    case DataNode::Kind::Number:
        return KnownValue::number(node.as<DataNode::Number>().value);
    case DataNode::Kind::String:
        return KnownValue::string(node.as<DataNode::String>().value);
    case DataNode::Kind::Hole:
    case DataNode::Kind::BigInt:
    case DataNode::Kind::Array:
    case DataNode::Kind::Object:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

KnownValue knownValueOfType(const Program& program, const Type& type)
{
    if (!type.isJavaScriptValue())
        return { };
    if (!type.hasObjects())
        return singlePrimitiveOf(type);
    if (type.primitives)
        return { };
    if (hasOnlyCallableObjects(type))
        return KnownValue::function();
    if (type.hasOnlyLayouts(program))
        return KnownValue::object();
    return { };
}

KnownValue knownValueOf(const Program& program, const Function& function, ValueID id)
{
    const Value& definition = function.at(sameObjectThroughCasts(function, id));
    if (primitiveConstantOpcodes.contains(definition.opcode)) {
        if (std::optional<ValueID> constant = constantThroughCasts(program, function, id))
            return evaluateConstant(function.at(*constant));
    }
    if (KnownValue allocation = knownValueOfAllocation(program, definition); allocation.isKnown())
        return allocation;
    return knownValueOfType(program, program.typeOf(function, id));
}

KnownBoolean evaluateEquality(KnownValue a, KnownValue b, EqualityKind equality)
{
    bool isStrict = equality == EqualityKind::Strict;
    if (!a.isKnown() || !b.isKnown())
        return KnownBoolean::Unknown;
    if (a.isNullish() || b.isNullish())
        return knownBoolean(isStrict ? a.kind() == b.kind() : a.isNullish() && b.isNullish());
    if (a.isNumber() && b.isNumber())
        return knownBoolean(a.asNumber() == b.asNumber());
    if (a.isObject() && b.isObject())
        return KnownBoolean::Unknown;
    if (a.kind() != b.kind())
        return isStrict ? KnownBoolean::False : KnownBoolean::Unknown;
    if (a.kind() == KnownValue::Kind::Boolean)
        return knownBoolean(a.asBoolean() == b.asBoolean());
    return knownBoolean(a.asString() == b.asString());
}

KnownBoolean evaluateTypeTest(const Program& program, const Type& source, const Type& tested)
{
    if (source.isNever())
        return KnownBoolean::Unknown;
    if (source.isSubsetOf(tested, program))
        return KnownBoolean::True;
    if (!source.overlaps(tested, program))
        return KnownBoolean::False;
    return KnownBoolean::Unknown;
}

Type typeOfKnownPrimitive(KnownValue value)
{
    switch (value.kind()) {
    case KnownValue::Kind::Undefined:
        return Type::fromPrimitives(Primitive::Undefined);
    case KnownValue::Kind::Null:
        return Type::fromPrimitives(Primitive::Null);
    case KnownValue::Kind::Boolean:
        return Type::fromPrimitives(value.asBoolean() ? Primitive::True : Primitive::False);
    case KnownValue::Kind::Integer:
        return Type::integer(Range::one(value.asInteger()));
    case KnownValue::Kind::Double:
        return Type::number(value.asNumber());
    case KnownValue::Kind::String:
        return Type::stringLiteral(value.asString());
    case KnownValue::Kind::Unknown:
    case KnownValue::Kind::Object:
    case KnownValue::Kind::Function:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

KnownBoolean evaluateTypeTest(const Program& program, KnownValue value, const Type& tested)
{
    switch (value.kind()) {
    case KnownValue::Kind::Unknown:
        return KnownBoolean::Unknown;
    case KnownValue::Kind::Undefined:
    case KnownValue::Kind::Null:
    case KnownValue::Kind::Boolean:
    case KnownValue::Kind::Integer:
    case KnownValue::Kind::Double:
    case KnownValue::Kind::String:
        return knownBoolean(typeOfKnownPrimitive(value).isSubsetOf(tested, program));
    case KnownValue::Kind::Object:
        if (!tested.hasObjects() || hasOnlyCallableObjects(tested))
            return KnownBoolean::False;
        return tested.anyObject ? KnownBoolean::True : KnownBoolean::Unknown;
    case KnownValue::Kind::Function:
        return evaluateTypeTest(program, Type::function(), tested);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static KnownBoolean evaluateStrictEquality(const Program& program, const Type& left, const Type& right)
{
    if ((left.primitives & Primitive::Number) && (right.primitives & Primitive::Number))
        return KnownBoolean::Unknown;
    if (left.overlaps(right, program))
        return KnownBoolean::Unknown;
    return KnownBoolean::False;
}

static KnownBoolean truthinessOf(const Program& program, const Type& type)
{
    static constexpr uint16_t falsyPrimitives = Primitive::UndefinedOrNull | Primitive::False;
    static constexpr uint16_t undecidedPrimitives = Primitive::Double | Primitive::String | Primitive::BigInt | Primitive::Absent;

    if (type.isNever() || !type.isJavaScriptValue())
        return KnownBoolean::Unknown;
    if (type.hasOnlyPrimitives(falsyPrimitives | Primitive::Integer) && type.integers.isSubsetOf(Range::one(0)))
        return KnownBoolean::False;
    if (type.primitives & (falsyPrimitives | undecidedPrimitives) || type.integers.contains(0))
        return KnownBoolean::Unknown;
    for (StringID literal : type.stringLiterals) {
        if (program.string(literal).empty())
            return KnownBoolean::Unknown;
    }
    return KnownBoolean::True;
}

KnownBoolean evaluateIsClosureOf(const Program& program, const Type& source, FunctionID function)
{
    if (source.singleFunction() == function && !source.mayBeUndefinedOrNull())
        return KnownBoolean::True;
    if (!source.overlaps(Type::forFunction(function), program))
        return KnownBoolean::False;
    return KnownBoolean::Unknown;
}

static bool mayContainNumbers(const Program& program, const Type& array)
{
    const ClassPart* part = array.singleBuiltinClass();
    if (!part || part->builtinClass != BuiltinClasses::Array || !part->contentsAreProven)
        return true;
    return program.at(part->contentsAt(program.builtins(), ContentsSlot::Element)).primitives & Primitive::Number;
}

// The storage kinds that are left for an array are no range of integers, so the type of an ArrayStorageKind cannot say them.
bool isRuledOutByDefinition(const Program& program, const Function& function, ValueID id, KnownValue candidate)
{
    const Value& definition = function.at(definitionThroughUpcasts(function, id));
    if (definition.opcode != Opcode::ArrayStorageKind || candidate != KnownValue::integer(static_cast<int64_t>(ArrayStorage::Double)))
        return false;
    return !mayContainNumbers(program, program.typeOf(function, definition.arrayOperand()));
}

static KnownBoolean evaluateComparisonByDefinitions(const Program& program, const Function& function, const Value& comparison)
{
    for (unsigned i = 0; i < 2; ++i) {
        if (isRuledOutByDefinition(program, function, comparison.operand(i), knownValueOf(program, function, comparison.operand(1 - i))))
            return KnownBoolean::False;
    }
    return KnownBoolean::Unknown;
}

KnownBoolean evaluateTestByTypes(const Program& program, const Function& function, const Value& test)
{
    auto typeOf = [&](ValueID id) -> const Type& {
        return program.typeOf(function, id);
    };
    switch (test.opcode) {
    case Opcode::TypeTest:
        return evaluateTypeTest(program, typeOf(test.valueOperand()), program.at(test.testedType()));
    case Opcode::StrictEq:
        if (evaluateStrictEquality(program, typeOf(test.leftOperand()), typeOf(test.rightOperand())) == KnownBoolean::False)
            return KnownBoolean::False;
        return evaluateComparisonByDefinitions(program, function, test);
    case Opcode::Eq:
        return evaluateComparisonByDefinitions(program, function, test);
    case Opcode::ToBoolean:
        return truthinessOf(program, typeOf(test.valueOperand()));
    case Opcode::IsClosureOf:
        return evaluateIsClosureOf(program, typeOf(test.valueOperand()), test.function());
    default:
        return KnownBoolean::Unknown;
    }
}

std::optional<int64_t> switchCaseOf(KnownValue value)
{
    switch (value.kind()) {
    case KnownValue::Kind::Integer:
        return value.asInteger();
    case KnownValue::Kind::Double:
        if (!value.asNumber())
            return 0;
        return std::nullopt;
    case KnownValue::Kind::Unknown:
    case KnownValue::Kind::Undefined:
    case KnownValue::Kind::Null:
    case KnownValue::Kind::Boolean:
    case KnownValue::Kind::String:
    case KnownValue::Kind::Object:
    case KnownValue::Kind::Function:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static KnownValue evaluateCast(const Program& program, const Value& value, KnownValue operand)
{
    switch (value.opcode) {
    case Opcode::Upcast:
    case Opcode::NarrowByField:
    case Opcode::AssumeType:
    case Opcode::CheckInitialized:
        return operand;
    case Opcode::Check:
        if (evaluateTypeTest(program, operand, program.at(value.castType())) != KnownBoolean::True)
            return { };
        return operand;
    case Opcode::RequireObjectCoercible:
        if (operand.isNullish())
            return { };
        return operand;
    case Opcode::RequireObject:
        if (!operand.isObject())
            return { };
        return operand;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static KnownValue evaluateTest(const Program& program, const Value& value, KnownValue operand)
{
    switch (value.opcode) {
    case Opcode::ToBoolean:
        return toKnownValue(operand.toBoolean(program));
    case Opcode::LogicalNot:
        if (operand.kind() != KnownValue::Kind::Boolean)
            return { };
        return KnownValue::boolean(!operand.asBoolean());
    case Opcode::TypeTest:
        return toKnownValue(evaluateTypeTest(program, operand, program.at(value.testedType())));
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

template<typename Number>
static bool evaluateRelation(Opcode opcode, Number left, Number right)
{
    switch (opcode) {
    case Opcode::Less:
        return left < right;
    case Opcode::LessEq:
        return left <= right;
    case Opcode::Greater:
        return left > right;
    case Opcode::GreaterEq:
        return left >= right;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static KnownValue evaluateComparison(Opcode opcode, KnownValue left, KnownValue right)
{
    switch (opcode) {
    case Opcode::StrictEq:
        return toKnownValue(evaluateEquality(left, right, EqualityKind::Strict));
    case Opcode::Eq:
        return toKnownValue(evaluateEquality(left, right, EqualityKind::Loose));
    case Opcode::Less:
    case Opcode::LessEq:
    case Opcode::Greater:
    case Opcode::GreaterEq:
        if (!left.isNumber() || !right.isNumber())
            return { };
        if (left.kind() == KnownValue::Kind::Integer && right.kind() == KnownValue::Kind::Integer)
            return KnownValue::boolean(evaluateRelation(opcode, left.asInteger(), right.asInteger()));
        return KnownValue::boolean(evaluateRelation(opcode, left.asNumber(), right.asNumber()));
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static bool hasInt32Magnitude(KnownValue value)
{
    return value.kind() == KnownValue::Kind::Integer && std::abs(value.asInteger()) <= std::numeric_limits<int32_t>::max();
}

static KnownValue evaluateAddOrSub(Opcode opcode, KnownValue left, KnownValue right)
{
    if (!left.isNumber() || !right.isNumber())
        return { };
    bool isAddition = opcode == Opcode::Add;
    if (hasInt32Magnitude(left) && hasInt32Magnitude(right))
        return KnownValue::integer(isAddition ? left.asInteger() + right.asInteger() : left.asInteger() - right.asInteger());
    return KnownValue::number(isAddition ? left.asNumber() + right.asNumber() : left.asNumber() - right.asNumber());
}

// The constant, where the sum is an integer plus a constant and the integer is the given one.
static std::optional<int64_t> distanceAddedTo(const Program& program, const Function& function, ValueID integer, ValueID sum)
{
    const Value& addition = function.at(sum);
    if (addition.opcode != Opcode::Add || addition.operand(0) != integer || !program.typeOf(function, integer).isInt32())
        return std::nullopt;
    const Value& constant = function.at(addition.operand(1));
    if (constant.opcode != Opcode::Integer || !hasInt32Magnitude(KnownValue::integer(constant.integer())))
        return std::nullopt;
    return constant.integer();
}

static KnownValue evaluateBitAnd(const Program& program, const Function& function, const Value& value, std::array<KnownValue, 2> operands)
{
    if (operands[0].isNumber() && operands[1].isNumber())
        return KnownValue::integer(toInt32(operands[0].asNumber()) & toInt32(operands[1].asNumber()));
    for (unsigned side = 0; side < 2; ++side) {
        if (operands[side].isNumber() && !toInt32(operands[side].asNumber()) && program.typeOf(function, value.operand(1 - side)).isNumber())
            return KnownValue::integer(0);
    }
    return { };
}

static KnownValue evaluateArithmetic(const Program& program, const Function& function, const Value& value, std::array<KnownValue, 2> operands)
{
    switch (value.opcode) {
    case Opcode::Sub:
        if (std::optional<int64_t> distance = distanceAddedTo(program, function, value.operand(1), value.operand(0)))
            return KnownValue::integer(*distance);
        [[fallthrough]];
    case Opcode::Add:
        return evaluateAddOrSub(value.opcode, operands[0], operands[1]);
    case Opcode::BitAnd:
        return evaluateBitAnd(program, function, value, operands);
    case Opcode::ToNumber:
    case Opcode::ToNumeric:
        if (!operands[0].isNumber())
            return { };
        return operands[0];
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static KnownValue evaluateStringLength(const Program& program, KnownValue string)
{
    if (string.kind() != KnownValue::Kind::String)
        return { };
    Vector<char16_t> buffer;
    return KnownValue::integer(program.strings.viewOf(string.asString(), buffer).length());
}

KnownValue evaluateOperation(const Program& program, const Function& function, const Value& value, std::array<KnownValue, 2> operands)
{
    RELEASE_ASSERT(opcodesWithKnownValueRules.contains(value.opcode));
    switch (value.opcode) {
    case Opcode::Undefined:
    case Opcode::Null:
    case Opcode::Boolean:
    case Opcode::Integer:
    case Opcode::Double:
    case Opcode::String:
        return evaluateConstant(value);
    case Opcode::Upcast:
    case Opcode::NarrowByField:
    case Opcode::Check:
    case Opcode::AssumeType:
    case Opcode::CheckInitialized:
    case Opcode::RequireObjectCoercible:
    case Opcode::RequireObject:
        return evaluateCast(program, value, operands[0]);
    case Opcode::ToBoolean:
    case Opcode::LogicalNot:
    case Opcode::TypeTest:
        return evaluateTest(program, value, operands[0]);
    case Opcode::StrictEq:
    case Opcode::Eq:
    case Opcode::Less:
    case Opcode::LessEq:
    case Opcode::Greater:
    case Opcode::GreaterEq:
        return evaluateComparison(value.opcode, operands[0], operands[1]);
    case Opcode::Add:
    case Opcode::Sub:
    case Opcode::BitAnd:
    case Opcode::ToNumber:
    case Opcode::ToNumeric:
        return evaluateArithmetic(program, function, value, operands);
    case Opcode::GetStringLength:
        return evaluateStringLength(program, operands[0]);
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static std::optional<String> textOf(const Program& program, KnownValue value)
{
    switch (value.kind()) {
    case KnownValue::Kind::Undefined:
        return String("undefined"_s);
    case KnownValue::Kind::Null:
        return String("null"_s);
    case KnownValue::Kind::Boolean:
        return String(value.asBoolean() ? "true"_s : "false"_s);
    case KnownValue::Kind::Integer:
    case KnownValue::Kind::Double:
        return String::number(value.asNumber());
    case KnownValue::Kind::String:
        return program.strings.toWTFString(value.asString());
    case KnownValue::Kind::Unknown:
    case KnownValue::Kind::Object:
    case KnownValue::Kind::Function:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

KnownValue evaluateStringOperation(const Program& program, StringArena& strings, const Function&, const Value& value, NOESCAPE const ScopedLambda<KnownValue(ValueID)>& operandValue)
{
    if (!opcodesCreatingKnownStrings.contains(value.opcode))
        return { };

    Vector<KnownValue, 4> operands;
    bool hasStringOperand = false;
    for (uint32_t operandIndex = 0; operandIndex < value.numberOfOperands(); ++operandIndex) {
        KnownValue operand = operandValue(value.operand(operandIndex));
        hasStringOperand |= operand.kind() == KnownValue::Kind::String;
        operands.append(operand);
    }
    if (value.opcode == Opcode::Add && !hasStringOperand)
        return { };
    if (operands.size() == 1 && hasStringOperand)
        return operands[0];

    StringBuilder result(OverflowPolicy::RecordOverflow);
    for (KnownValue operand : operands) {
        std::optional<String> text = textOf(program, operand);
        if (!text)
            return { };
        result.append(*text);
    }
    if (result.hasOverflowed())
        return { };
    return KnownValue::string(strings.intern(StringView(result)));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
