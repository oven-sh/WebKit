/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTCurrentWork.h"
#include "AOTDecidedBranches.h"
#include "AOTEdgeResults.h"
#include "AOTEffects.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsDescription.h"
#include "AOTEffectsOfOperands.h"
#include "AOTFlowResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTGuardAvailability.h"
#include "AOTIRBuilder.h"
#include "AOTInt32Conversions.h"
#include "AOTKnownClosures.h"
#include "AOTKnownValue.h"
#include "AOTObservableOperations.h"
#include "AOTProgramPasses.h"
#include "AOTPropertyAccessElements.h"
#include "AOTPropertyAccessReceivers.h"
#include "AOTRegExpFlags.h"
#include "AOTResultRules.h"
#include "AOTRewrite.h"
#include "AOTRuleSupport.h"
#include "AOTSchemaOperandIndexes.h"
#include "AOTScratchRealm.h"
#include "AOTSort.h"
#include "AOTStringArena.h"
#include "AOTStringTable.h"
#include "AOTStronglyConnectedComponents.h"
#include "AOTSwitchWindow.h"
#include "AOTThreshold.h"
#include "AOTTypePropagation.h"
#include "AOTTypeTestSite.h"
#include "AOTValueIdentity.h"
#include "AOTValueSubstitution.h"
#include "AOTVector.h"
#include "AOTVirtualObject.h"
#include "JSArrayIterator.h"
#include "JSSetIterator.h"
#include "JSCInlines.h"
#include "MathCommon.h"
#include "PureNaN.h"
#include "TypeofType.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/ScopedLambda.h>
#include <wtf/text/StringBuilder.h>

namespace JSC { namespace AOT { namespace Typed {

// KnownValue.

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

void becomeConstant(const Program& program, Function& function, TypeArena& types, ValueID id, KnownValue constant)
{
    Value& value = function.edit(id);
    switch (constant.kind()) {
    case KnownValue::Kind::Undefined:
        value.become(Opcode::Undefined);
        break;
    case KnownValue::Kind::Null:
        value.become(Opcode::Null);
        break;
    case KnownValue::Kind::Boolean:
        value.become(Opcode::Boolean);
        value.setBoolean(constant.asBoolean());
        break;
    case KnownValue::Kind::Integer:
        value.become(Opcode::Integer);
        value.setInteger(constant.asInteger());
        break;
    case KnownValue::Kind::Double:
        value.become(Opcode::Double);
        value.setNumber(constant.asNumber());
        break;
    case KnownValue::Kind::String:
        value.become(Opcode::String);
        value.setString(constant.asString());
        break;
    case KnownValue::Kind::Unknown:
    case KnownValue::Kind::Object:
    case KnownValue::Kind::Function:
        RELEASE_ASSERT_NOT_REACHED();
    }
    value.type = resultTypeOf(program, function, value, types);
    value.setExpected({ });
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
        return type.integers.minimum == type.integers.maximum ? KnownValue::integer(type.integers.minimum) : KnownValue();
    case Primitive::LiteralString:
        return type.stringLiterals.size() == 1 ? KnownValue::string(type.stringLiterals[0]) : KnownValue();
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

KnownValue knownValueOfType(const Type& type)
{
    if (!type.isJavaScriptValue())
        return { };
    if (!type.hasObjects())
        return singlePrimitiveOf(type);
    if (type.primitives)
        return { };
    if (hasOnlyCallableObjects(type))
        return KnownValue::function();
    return type.hasOnlyLayouts() ? KnownValue::object() : KnownValue();
}

KnownValue knownValueOf(const Program& program, const Function& function, ValueID id)
{
    const Value& definition = function.at(sameObjectThroughCasts(function, id));
    if (primitiveConstantOpcodes.contains(definition.opcode)) {
        if (std::optional<ValueID> constant = constantThroughCasts(program, function, id))
            return evaluateConstant(function.at(*constant));
    }
    if (definition.opcode == Opcode::NewClosure || definition.opcode == Opcode::NewClass)
        return KnownValue::function();
    if (resultRuleOf(definition.opcode) == ResultRule::Allocation && program.at(definition.type).isJavaScriptValue())
        return KnownValue::object();
    return knownValueOfType(program.typeOf(function, id));
}

enum class EqualityKind : uint8_t { Strict, Loose };

static KnownBoolean evaluateEquality(KnownValue a, KnownValue b, EqualityKind equality)
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
    return source.overlaps(tested, program) ? KnownBoolean::Unknown : KnownBoolean::False;
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

static bool mayBeStrictlyEqual(const Program& program, const Type& left, const Type& right)
{
    return ((left.primitives & Primitive::Number) && (right.primitives & Primitive::Number)) || left.overlaps(right, program);
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
    return source.overlaps(Type::forFunction(function), program) ? KnownBoolean::Unknown : KnownBoolean::False;
}

static bool mayContainNumbers(const Program& program, const Type& array)
{
    const ClassPart* part = array.singleBuiltinClass();
    if (!part || part->builtinClass != BuiltinClasses::Array || !part->contentsAreProven)
        return true;
    return program.at(*part->provenContentsAt(program.builtins(), ContentsSlot::Element)).primitives & Primitive::Number;
}

// The storage kinds that are left for an array are no range of integers, so the type of an ArrayStorageKind cannot say them.
bool isRuledOutByDefinition(const Program& program, const Function& function, ValueID id, KnownValue candidate)
{
    const Value& definition = function.at(definitionThroughUpcasts(function, id));
    if (definition.opcode != Opcode::ArrayStorageKind || candidate != KnownValue::integer(static_cast<int64_t>(ArrayStorage::Double)))
        return false;
    return !mayContainNumbers(program, program.typeOf(function, definition.arrayOperand()));
}

KnownBoolean evaluateTestByTypes(const Program& program, const Function& function, const Value& test)
{
    auto typeOf = [&](ValueID id) -> const Type& { return program.typeOf(function, id); };
    switch (test.opcode) {
    case Opcode::TypeTest:
        return evaluateTypeTest(program, typeOf(test.valueOperand()), program.at(test.testedType()));
    case Opcode::StrictEq:
        if (!mayBeStrictlyEqual(program, typeOf(test.leftOperand()), typeOf(test.rightOperand())))
            return KnownBoolean::False;
        [[fallthrough]];
    case Opcode::Eq:
        for (unsigned i = 0; i < 2; ++i) {
            if (isRuledOutByDefinition(program, function, test.operand(i), knownValueOf(program, function, test.operand(1 - i))))
                return KnownBoolean::False;
        }
        return KnownBoolean::Unknown;
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

static bool hasInt32Magnitude(KnownValue value)
{
    return value.kind() == KnownValue::Kind::Integer && std::abs(value.asInteger()) <= std::numeric_limits<int32_t>::max();
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

KnownValue evaluateOperation(const Program& program, const Function& function, const Value& value, std::array<KnownValue, 2> operands)
{
    RELEASE_ASSERT(hasKnownValueRuleOf(value.opcode));
    auto [left, right] = operands;
    bool areNumbers = left.isNumber() && right.isNumber();
    switch (value.opcode) {
    case Opcode::Undefined:
    case Opcode::Null:
    case Opcode::Boolean:
    case Opcode::Integer:
    case Opcode::Double:
    case Opcode::String:
        return evaluateConstant(value);
    case Opcode::Upcast:
    case Opcode::Expect:
    case Opcode::NarrowByField:
    case Opcode::AssumeType:
    case Opcode::CheckInitialized:
        return left;
    case Opcode::Check:
        return evaluateTypeTest(program, left, program.at(value.expectedType().predicted())) == KnownBoolean::True ? left : KnownValue();
    case Opcode::RequireObjectCoercible:
        return left.isNullish() ? KnownValue() : left;
    case Opcode::RequireObject:
        return left.isObject() ? left : KnownValue();
    case Opcode::ToBoolean:
        return toKnownValue(left.toBoolean(program));
    case Opcode::LogicalNot:
        return left.kind() == KnownValue::Kind::Boolean ? KnownValue::boolean(!left.asBoolean()) : KnownValue();
    case Opcode::TypeTest:
        return toKnownValue(evaluateTypeTest(program, left, program.at(value.testedType())));
    case Opcode::IsPristine: {
        const Type& operand = program.typeOf(function, value.valueOperand());
        return operand.singleBuiltinClass() && operand.singleBuiltinClass()->builtinClass == value.builtinClass() && !operand.mayBeUndefinedOrNull() && pristineAspectsProvenBy(operand, value.builtinClass()).containsAll(value.aspects()) ? KnownValue::boolean(true) : KnownValue();
    }
    case Opcode::IsPristineArray: {
        const Type& list = program.typeOf(function, value.valueOperand());
        bool isList = list.singleBuiltinClass() && list.singleBuiltinClass()->builtinClass == BuiltinClasses::Array && !list.mayBeUndefinedOrNull();
        bool isSet = list.singleBuiltinClass() && list.singleBuiltinClass()->builtinClass == BuiltinClasses::Set && !list.mayBeUndefinedOrNull();
        if (isSet && pristineAspectsProvenBy(list, BuiltinClasses::Set).contains(PristineAspects::Names))
            return KnownValue::boolean(true);
        return isList && pristineAspectsProvenBy(list, BuiltinClasses::Array).containsAll({ PristineAspects::Storage, PristineAspects::Names }) ? KnownValue::boolean(true) : KnownValue();
    }
    case Opcode::StrictEq:
        return toKnownValue(evaluateEquality(left, right, EqualityKind::Strict));
    case Opcode::Eq:
        return toKnownValue(evaluateEquality(left, right, EqualityKind::Loose));
    case Opcode::Less:
    case Opcode::LessEq:
    case Opcode::Greater:
    case Opcode::GreaterEq:
        if (!areNumbers) {
            // Where the two pairs of ends that lie furthest apart compare alike, so does every pair between them.
            const Type& leftType = program.typeOf(function, value.operand(0));
            const Type& rightType = program.typeOf(function, value.operand(1));
            if (!leftType.isInteger() || !rightType.isInteger())
                return { };
            bool answer = evaluateRelation(value.opcode, leftType.integers.minimum, rightType.integers.maximum);
            return answer == evaluateRelation(value.opcode, leftType.integers.maximum, rightType.integers.minimum) ? KnownValue::boolean(answer) : KnownValue();
        }
        if (left.kind() == KnownValue::Kind::Integer && right.kind() == KnownValue::Kind::Integer)
            return KnownValue::boolean(evaluateRelation(value.opcode, left.asInteger(), right.asInteger()));
        return KnownValue::boolean(evaluateRelation(value.opcode, left.asNumber(), right.asNumber()));
    case Opcode::Sub:
        if (std::optional<int64_t> distance = distanceAddedTo(program, function, value.operand(1), value.operand(0)))
            return KnownValue::integer(*distance);
        if (!areNumbers)
            return { };
        if (hasInt32Magnitude(left) && hasInt32Magnitude(right))
            return KnownValue::integer(left.asInteger() - right.asInteger());
        return KnownValue::computedNumber(left.asNumber() - right.asNumber());
    case Opcode::Add:
        if (!areNumbers)
            return { };
        if (hasInt32Magnitude(left) && hasInt32Magnitude(right))
            return KnownValue::integer(left.asInteger() + right.asInteger());
        return KnownValue::computedNumber(left.asNumber() + right.asNumber());
    case Opcode::BitAnd:
        if (areNumbers)
            return KnownValue::integer(toInt32(left.asNumber()) & toInt32(right.asNumber()));
        for (unsigned side = 0; side < 2; ++side) {
            if (operands[side].isNumber() && !toInt32(operands[side].asNumber()) && program.typeOf(function, value.operand(1 - side)).isNumber())
                return KnownValue::integer(0);
        }
        return { };
    case Opcode::ToNumber:
    case Opcode::ToNumeric:
        return left.isNumber() ? left : KnownValue();
    case Opcode::GetStringLength: {
        Vector<char16_t> buffer;
        if (left.kind() == KnownValue::Kind::String)
            return KnownValue::integer(program.strings.viewOf(left.asString(), buffer).length());
        // Strings that all have the same length have that length, whichever one it is.
        const Type& type = program.typeOf(function, value.operand(0));
        if (!type.hasLiteralStrings() || !type.hasOnlyPrimitives(Primitive::LiteralString))
            return { };
        std::optional<unsigned> length;
        for (StringID literal : type.stringLiterals) {
            unsigned lengthOfLiteral = program.strings.viewOf(literal, buffer).length();
            if (length && *length != lengthOfLiteral)
                return { };
            length = lengthOfLiteral;
        }
        return length ? KnownValue::integer(*length) : KnownValue();
    }
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
        operands.append(operandValue(value.operand(operandIndex)));
        hasStringOperand |= operands.last().kind() == KnownValue::Kind::String;
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

// CompactBlocks.

PassOutcome compactBlocks(FunctionPassContext& context)
{
    if (context.isLastStepOfSchedule && !context.build.options().verifiesAfterEveryPass)
        return compactFunction(context.function) ? PassOutcome::Changed : PassOutcome::Unchanged;

    Renumbering renumbering;
    if (!context.pendingTyping.compact(context.function, &renumbering))
        return PassOutcome::Unchanged;
    context.analyses.didRenumber(renumbering);
    context.keptAnalyses = allAnalyses;
    return PassOutcome::Changed;
}

// EliminateDeadCode.

static bool writesOnlyObjects(const Effects& effects)
{
    return !effects.mayExitSideways() && effects.writes.containsOnly(perObjectHeaps | bookkeepingHeaps);
}

namespace {

// What becomes of a value that nothing uses. The row of most operations answers for all their values.
enum class UnusedValueRule : uint8_t {
    DependsOnEffects,
    IsRemoved,
    Stays,
    // A store into one of its operands, and nothing else.
    StaysWithWrittenObject,
};

struct OpcodeRules {
    UnusedValueRule unusedValue { UnusedValueRule::DependsOnEffects };
    bool writesOnlyNamedOperands { false };
    bool mayHaveFreshResult { true };
};

static UnusedValueRule unusedValueRuleOf(Opcode opcode, bool writesOnlyNamedOperands)
{
    const OpcodeEffectFacts& facts = effectFactsOf(opcode);
    Effects atMost = declaredBoundOf(opcode);
    if (facts.staysWithinDeclaredBound && atMost.isRemovableWhenUnused())
        return UnusedValueRule::IsRemoved;

    // The questions below ask which heaps are written and whether there is a way out, so one way out stands for all of them.
    Effects atLeast = Effects::none();
    atLeast.writes = facts.writtenHeaps;
    atLeast.mayThrow = facts.mayExitSideways;
    if (atLeast.isRemovableWhenUnused())
        return UnusedValueRule::DependsOnEffects;
    if (!writesOnlyNamedOperands || !writesOnlyObjects(atLeast))
        return UnusedValueRule::Stays;
    return facts.staysWithinDeclaredBound && writesOnlyObjects(atMost) ? UnusedValueRule::StaysWithWrittenObject : UnusedValueRule::DependsOnEffects;
}

using RulesOfOpcodes = std::array<OpcodeRules, numberOfOpcodes>;

static const RulesOfOpcodes& rulesOfOpcodes()
{
    static const RulesOfOpcodes rules = [] {
        RulesOfOpcodes result { };
        for (Opcode member : allMembersOf<Opcode>) {
            bool writesOnlyNamedOperands = confinementOf(member) == OperandConfinement::WritesOnlyNamedOperands;
            result[static_cast<unsigned>(member)] = { unusedValueRuleOf(member, writesOnlyNamedOperands), writesOnlyNamedOperands, resultMayBeFresh(member) };
        }
        return result;
    }();
    return rules;
}

// One bit a value, in line for a function of up to 1,024 values.
struct BitOfEachValue {
    explicit BitOfEachValue(size_t numberOfValues)
        : words(FillWith { }, (numberOfValues + 63) / 64, 0)
    {
    }

    bool get(uint32_t index) const { return words[index >> 6] >> (index & 63) & 1; }
    void set(uint32_t index) { words[index >> 6] |= 1ull << (index & 63); }
    bool testAndSet(uint32_t index)
    {
        uint64_t& word = words[index >> 6];
        bool wasSet = word >> (index & 63) & 1;
        word |= 1ull << (index & 63);
        return wasSet;
    }

    Vector<uint64_t, 16> words;
};

// No stack walk starts where control only falls through.
static bool runsToItsEnd(TerminatorKind kind)
{
    switch (kind) {
    case TerminatorKind::Jump:
    case TerminatorKind::Branch:
    case TerminatorKind::TestAndBranch:
    case TerminatorKind::Switch:
    case TerminatorKind::SwitchOnShape:
    case TerminatorKind::Return:
    case TerminatorKind::Unreachable:
        return true;
    case TerminatorKind::SwitchString:
    case TerminatorKind::ReturnCall:
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
    case TerminatorKind::TryCallInline:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool keepsCalleeInFrame(Opcode opcode) { return opcode == Opcode::KeepCalleeInFrame || opcode == Opcode::KeepEnvironmentOfCalleeInFrame; }

// A call and the operation that keeps its callee share one position in one inline frame.
static uint64_t keyOfCall(uint32_t position, uint32_t inlineFrame) { return static_cast<uint64_t>(position) << 32 | inlineFrame; }

// Only a stack walk needs to know which function object a call entered, and inside an inlined callee that can only happen where something may exit sideways.
// If those places are expected to run less often than the head of the inlined body, the function object is kept right before them.
struct KeptCalleeMover {
    struct Keep {
        ValueID id;
        ProgramPoint point;
        uint64_t key { 0 };
        bool stays { false };
        // In walk order.
        Vector<ProgramPoint, 2> queryPoints { };
    };
    struct Question {
        uint64_t key { 0 };
        ProgramPoint point;
    };
    struct StateOfKey {
        BlockID closedIn;
        bool stays { false };
    };

    FunctionPassContext& context;
    Function& function { context.function };
    FunctionAnalyses& analyses { context.analyses };
    Vector<Keep, 8> keeps { };
    Vector<Question, 16> questions { };
    UncheckedKeyHashMap<uint64_t, StateOfKey, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> stateOfKey { };
    Vector<uint64_t, 4> lastKeyOfWord { };

    uint32_t wordOf(uint64_t key) const
    {
        uint32_t depth = 0;
        for (uint32_t frame = static_cast<uint32_t>(key); frame; frame = function.inlineFrames[frame].parent)
            ++depth;
        return depth;
    }

    // Calls at the same inline depth share a word. A call whose places in a block surround those of another call keeps its callee where it is.
    void didMeet(uint64_t key, BlockID block)
    {
        uint32_t word = wordOf(key);
        while (lastKeyOfWord.size() <= word)
            lastKeyOfWord.append(std::numeric_limits<uint64_t>::max() - 1);
        if (lastKeyOfWord[word] == key)
            return;
        if (auto last = stateOfKey.find(lastKeyOfWord[word]); last != stateOfKey.end())
            last->value.closedIn = block;
        StateOfKey& state = stateOfKey.add(key, StateOfKey { }).iterator->value;
        state.stays |= state.closedIn == block;
        lastKeyOfWord[word] = key;
    }

    void collect()
    {
        for (BlockID blockID : analyses.cfg().reversePostOrder()) {
            const Block& block = function.at(blockID);
            lastKeyOfWord.shrink(0);
            auto ask = [&](uint32_t frame, uint32_t index) {
                for (; frame; frame = function.inlineFrames[frame].parent) {
                    const InlineFrame& record = function.inlineFrames[frame];
                    if (!record.calleeIsKeptInFrame)
                        continue;
                    uint64_t key = keyOfCall(record.callPosition, record.parent);
                    didMeet(key, blockID);
                    questions.append({ key, { blockID, index } });
                }
            };
            for (uint32_t index = 0; index < block.values.size(); ++index) {
                ValueID id = block.values[index];
                const Value& value = function.at(id);
                if (keepsCalleeInFrame(value.opcode)) {
                    uint64_t key = keyOfCall(value.position, value.inlineFrame);
                    didMeet(key, blockID);
                    keeps.append({ id, { blockID, index }, key });
                    continue;
                }
                // The call itself needs it, right after the operation that keeps its callee.
                if (value.opcode == Opcode::CallDirect && value.calleeIsKeptInFrame())
                    stateOfKey.add(keyOfCall(value.position, value.inlineFrame), StateOfKey { }).iterator->value.stays = true;
                if (value.inlineFrame && analyses.mayExitSidewaysOnAnyEdge(id))
                    ask(value.inlineFrame, index);
            }
            if (block.terminator.inlineFrame && !runsToItsEnd(block.terminator.kind))
                ask(block.terminator.inlineFrame, block.values.size());
        }
    }

    bool standsInFrontOf(const Keep& keep, ProgramPoint point)
    {
        return keep.point.block == point.block ? keep.point.index < point.index : analyses.dominators().strictlyDominates(keep.point.block, point.block);
    }

    void assignQuestions()
    {
        AOT::stableSort(keeps, [](const Keep& a, const Keep& b) { return a.key < b.key; });
        for (const Question& question : questions) {
            auto [first, last] = std::ranges::equal_range(keeps, question.key, { }, &Keep::key);
            Keep* nearest = nullptr;
            for (Keep& keep : std::ranges::subrange(first, last)) {
                if (standsInFrontOf(keep, question.point) && (!nearest || standsInFrontOf(*nearest, keep.point)))
                    nearest = &keep;
            }
            // Paths from several of them meet before the place.
            if (!nearest) {
                stateOfKey.add(question.key, StateOfKey { }).iterator->value.stays = true;
                continue;
            }
            nearest->queryPoints.append(question.point);
        }
    }

    bool run()
    {
        if (std::ranges::none_of(function.inlineFrames, &InlineFrame::calleeIsKeptInFrame))
            return false;
        collect();
        if (keeps.isEmpty())
            return false;
        assignQuestions();

        IRBuilder builder(analyses.context(), function, context.types, &analyses);
        Vector<const Keep*, 8> moved;
        for (Keep& keep : keeps) {
            if (keep.stays || stateOfKey.get(keep.key).stays)
                continue;
            double frequencyAtQueries = 0;
            BlockID lastBlock;
            for (ProgramPoint point : keep.queryPoints) {
                if (point.block != std::exchange(lastBlock, point.block))
                    frequencyAtQueries += analyses.blockFrequencies().frequencyOf(point.block).predicted();
            }
            if (!keep.queryPoints.isEmpty() && frequencyAtQueries >= analyses.blockFrequencies().frequencyOf(keep.point.block).predicted())
                continue;
            moved.append(&keep);
        }
        if (moved.isEmpty())
            return false;

        for (const Keep* keep : moved) {
            const Value original = function.at(keep->id);
            builder.setOrigin(original.origin());
            BlockID lastBlock;
            for (ProgramPoint point : keep->queryPoints) {
                if (point.block != std::exchange(lastBlock, point.block))
                    builder.insertBefore(point, Value(original));
            }
        }
        builder.applyInsertions();

        AOT::sort(moved, { }, [](const Keep* keep) { return keep->point.block; });
        for (size_t first = 0; first < moved.size();) {
            BlockID block = moved[first]->point.block;
            BitVector erased;
            const auto& values = function.at(block).values;
            for (; first < moved.size() && moved[first]->point.block == block; ++first) {
                size_t index = values.find(moved[first]->id);
                RELEASE_ASSERT(index != notFound);
                erased.set(index);
            }
            builder.eraseValues(block, erased);
        }
        return true;
    }
};

// ONE WALK BACKWARD, block by block against the reverse post order and value by value against the order in the block.
struct DeadCodeEliminator {
    static constexpr uint32_t noWaitingStore = UINT32_MAX;
    // The rank of the block in reverse post order, and the index in the block.
    using Position = std::pair<size_t, size_t>;

    struct WaitingStore {
        ValueID receiver;
        ValueID store;
        // With the same receiver.
        uint32_t previous { noWaitingStore };
    };

    FunctionPassContext& context;
    Function& function { context.function };
    FunctionAnalyses& analyses { context.analyses };
    const CFG& cfg { analyses.cfg() };
    const RulesOfOpcodes& rulesOfOpcode { rulesOfOpcodes() };
    BitOfEachValue live { function.values.size() };
    BitOfEachValue needsAttentionWhenMarked { function.values.size() };
    Vector<ValueID, 64> worklist { };
    Vector<WaitingStore, 16> waitingStores { };
    UncheckedKeyHashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> lastWaitingStoreInto { };
    std::optional<Position> firstFreshAllocation { };
    // The position and inline frame of each, in ascending order.
    std::optional<Vector<uint64_t, 8>> callsWithQueriedCallee { };
    Vector<ValueLocation> parameterLocations { };
    Vector<Vector<ValueID, 2>> storesInto { };
    BitVector isActive { };
    ObservableOperations observable { analyses.effectContext(), function };
    bool tookProofsFromEdges { false };
    bool madeEdgesAlikeOrBlocksEmpty { false };
    bool tookArgumentsFromEmptyBlocks { false };
    const bool checksRules { context.build.options().verifiesAfterEveryPass };

    void mark(ValueID id)
    {
        if (!live.testAndSet(id.index()) && needsAttentionWhenMarked.get(id.index()))
            worklist.append(id);
    }

    void markOperandsOf(const Value& value)
    {
        value.forEachOperand([&](ValueID operand, uint32_t) { mark(operand); });
    }

    void walkBackward()
    {
        for (ValueID parameter : function.at(cfg.root()).parameters)
            live.set(parameter.index());

        std::span<const BlockID> order = cfg.reversePostOrder();
        for (size_t rank = order.size(); rank--;) {
            const Block& block = function.at(order[rank]);
            block.forEachTarget([&](const Target& target) {
                std::span<const ValueID> parameters = function.at(target.block).parameters.span();
                std::span<const ValueID> arguments = target.arguments.span();
                for (size_t index = 0; index < parameters.size(); ++index) {
                    if (!live.get(parameters[index].index()))
                        needsAttentionWhenMarked.set(parameters[index].index());
                    else if (index < arguments.size())
                        mark(arguments[index]);
                }
            });
            block.terminator.Instruction::forEachChild([&](ValueID operand) { mark(operand); });
            if (block.subject)
                mark(block.subject);
            std::span<const ValueID> values = block.values.span();
            for (size_t index = values.size(); index--;) {
                ValueID id = values[index];
                const Value& value = function.at(id);
                const OpcodeRules& rules = rulesOfOpcode[static_cast<unsigned>(value.opcode)];
                if (checksRules) [[unlikely]]
                    checkRules(id, rules);

                if (!live.get(id.index())) {
                    if (!staysUnused(id, value, rules, { rank, index })) {
                        needsAttentionWhenMarked.set(id.index());
                        continue;
                    }
                    live.set(id.index());
                    if (needsAttentionWhenMarked.get(id.index()))
                        markStoresThatWaitFor(id);
                }
                markOperandsOf(value);
            }
            if (!worklist.isEmpty())
                propagate();
        }
    }

    bool staysUnused(ValueID id, const Value& value, const OpcodeRules& rules, Position position)
    {
        // It keeps nothing alive: there is nothing to let go of where nothing else reads the value, or where the value names no cell.
        if (value.opcode == Opcode::ReleaseHint) {
            const Type& released = context.program.typeOf(function, value.valueOperand());
            bool mayNameCell = !released.isJavaScriptValue() || released.hasObjects() || released.primitives & cellPrimitives;
            return mayNameCell && !waitsFor(value.valueOperand(), id);
        }

        switch (rules.unusedValue) {
        case UnusedValueRule::IsRemoved:
            return false;
        case UnusedValueRule::Stays:
            if (keepsCalleeInFrame(value.opcode))
                return isCalleeQueried(value);
            // All that the look at a heritage does is throw, or run a getter. The type of the heritage may rule out both.
            return value.opcode != Opcode::ResolveHeritage || !observable.neitherThrowsNorRunsCode(id);
        case UnusedValueRule::StaysWithWrittenObject:
            return !waitsForWrittenObject(id, value, position);
        case UnusedValueRule::DependsOnEffects:
            break;
        }

        // The effects of a value only matter while nothing live uses it.
        if (analyses.isRemovableWhenUnused(id))
            return false;
        if (!rules.writesOnlyNamedOperands)
            return true;
        const EffectsOnEdges& effects = analyses.effectsOf(id);
        return !writesOnlyObjects(effects.normal) || !writesOnlyObjects(effects.handler) || !waitsForWrittenObject(id, value, position);
    }

    // A stack walk needs to know which function object a call entered. No stack walk starts inside an inlined callee in which nothing exits sideways.
    bool isCalleeQueried(const Value& keep)
    {
        if (!callsWithQueriedCallee) {
            callsWithQueriedCallee.emplace();
            BitVector hasExit(function.inlineFrames.size());
            auto didFindExitFrom = [&](uint32_t frame) {
                for (; frame && !hasExit.quickSet(frame); frame = function.inlineFrames[frame].parent) { }
            };
            for (BlockID blockID : cfg.reversePostOrder()) {
                const Block& block = function.at(blockID);
                for (ValueID id : block.values) {
                    const Value& value = function.at(id);
                    if (value.opcode == Opcode::CallDirect && value.calleeIsKeptInFrame())
                        callsWithQueriedCallee->append(keyOfCall(value.position, value.inlineFrame));
                    if (value.inlineFrame && !hasExit.quickGet(value.inlineFrame) && analyses.mayExitSidewaysOnAnyEdge(id))
                        didFindExitFrom(value.inlineFrame);
                }
                if (!runsToItsEnd(block.terminator.kind))
                    didFindExitFrom(block.terminator.inlineFrame);
            }
            for (uint32_t frame = 1; frame < function.inlineFrames.size(); ++frame) {
                if (function.inlineFrames[frame].calleeIsKeptInFrame && hasExit.quickGet(frame))
                    callsWithQueriedCallee->append(keyOfCall(function.inlineFrames[frame].callPosition, function.inlineFrames[frame].parent));
            }
            std::ranges::sort(*callsWithQueriedCallee);
        }
        return std::ranges::binary_search(*callsWithQueriedCallee, keyOfCall(keep.position, keep.inlineFrame));
    }

    void checkRules(ValueID id, const OpcodeRules& rules)
    {
        RELEASE_ASSERT(rules.mayHaveFreshResult || freshnessOf(analyses.effectContext(), function, function.at(id)) != ResultFreshness::Fresh);

        Effects effects = analyses.effectsOf(id).onAnyEdge();
        bool isStoreIntoOperand = rules.writesOnlyNamedOperands && writesOnlyObjects(effects);
        switch (rules.unusedValue) {
        case UnusedValueRule::DependsOnEffects:
            return;
        case UnusedValueRule::IsRemoved:
            RELEASE_ASSERT(effects.isRemovableWhenUnused());
            return;
        case UnusedValueRule::Stays:
            RELEASE_ASSERT(!effects.isRemovableWhenUnused() && !isStoreIntoOperand);
            return;
        case UnusedValueRule::StaysWithWrittenObject:
            RELEASE_ASSERT(!effects.isRemovableWhenUnused() && isStoreIntoOperand);
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    bool waitsForWrittenObject(ValueID store, const Value& value, Position position)
    {
        // In front of the first allocation there is no object that a store could be alone in knowing. It is looked for when the first store asks.
        if (!firstFreshAllocation)
            firstFreshAllocation = findFirstFreshAllocation();
        if (*firstFreshAllocation > position)
            return false;

        std::optional<uint32_t> writtenOperand;
        bool mayWriteOtherObjects = false;
        value.forEachOperand([&](ValueID, uint32_t operandIndex) {
            OperandUses uses = summarizeOperand(analyses.effectContext(), function, value, operandIndex).uses;
            uint32_t declaredIndex = std::min(operandIndex, value.numberOfNamedOperands());
            if (!isWrittenOperand(value.opcode, declaredIndex) && !uses.containsAny(operandWritingUses))
                return;
            mayWriteOtherObjects |= writtenOperand.has_value() || uses.containsAny(unconfinedUses);
            writtenOperand = operandIndex;
        });
        ValueID receiver = writtenOperand && !mayWriteOtherObjects ? value.operand(*writtenOperand) : ValueID();
        return receiver && waitsFor(receiver, store);
    }

    bool waitsFor(ValueID receiver, ValueID store)
    {
        if (live.get(receiver.index()))
            return false;

        auto last = lastWaitingStoreInto.add(receiver.index(), noWaitingStore);
        waitingStores.append({ receiver, store, last.iterator->value });
        last.iterator->value = waitingStores.size() - 1;
        needsAttentionWhenMarked.set(receiver.index());
        return true;
    }

    // Behind every position where there is none.
    Position findFirstFreshAllocation() const
    {
        std::span<const BlockID> order = cfg.reversePostOrder();
        for (size_t rank = 0; rank < order.size(); ++rank) {
            std::span<const ValueID> values = function.at(order[rank]).values.span();
            for (size_t index = 0; index < values.size(); ++index) {
                const Value& value = function.at(values[index]);
                if (rulesOfOpcode[static_cast<unsigned>(value.opcode)].mayHaveFreshResult && freshnessOf(analyses.effectContext(), function, value) == ResultFreshness::Fresh)
                    return { rank, index };
            }
        }
        return { std::numeric_limits<size_t>::max(), 0 };
    }

    void propagate()
    {
        const EscapeAnalysis* escape = storesInto.isEmpty() ? nullptr : &analyses.escape();
        while (!worklist.isEmpty()) {
            ValueID id = worklist.takeLast();
            markInputsOf(id);
            if (!waitingStores.isEmpty())
                markStoresThatWaitFor(id);
            if (escape)
                activateObjects(escape->pointsTo(id));
        }
    }

    void markStoresThatWaitFor(ValueID receiver)
    {
        auto last = lastWaitingStoreInto.find(receiver.index());
        if (last == lastWaitingStoreInto.end())
            return;
        for (uint32_t index = last->value; index != noWaitingStore; index = waitingStores[index].previous)
            mark(waitingStores[index].store);
    }

    // Whether the escape analysis was asked.
    bool findObjectsOfWaitingStores()
    {
        auto waits = [&](const WaitingStore& waiting) { return !live.get(waiting.store.index()) && function.at(waiting.store).opcode != Opcode::ReleaseHint; };
        if (!std::ranges::any_of(waitingStores, waits))
            return false;

        const EscapeAnalysis& escape = analyses.escape();
        for (const WaitingStore& waiting : waitingStores) {
            if (!waits(waiting))
                continue;
            std::optional<ObjectID> object = escape.gaveUp() ? std::nullopt : escape.singleObject(waiting.receiver);
            if (!object || escape.at(*object).kind != ObjectKind::Allocation || escape.at(*object).state != EscapeState::DoesNotEscape) {
                mark(waiting.store);
                continue;
            }
            if (storesInto.isEmpty()) {
                storesInto.grow(escape.numberOfObjects());
                isActive.ensureSize(escape.numberOfObjects());
            }
            storesInto[object->index()].append(waiting.store);
        }
        for (size_t wordIndex = 0; !storesInto.isEmpty() && wordIndex < live.words.size(); ++wordIndex) {
            for (uint64_t word = live.words[wordIndex]; word; word &= word - 1)
                activateObjects(escape.pointsTo(ValueID(static_cast<uint32_t>(wordIndex * 64 + std::countr_zero(word)))));
        }
        return true;
    }

    void markInputsOf(ValueID id)
    {
        const Value& value = function.at(id);
        if (value.opcode != Opcode::Parameter) {
            markOperandsOf(value);
            return;
        }
        if (parameterLocations.isEmpty() && !analyses.computed().contains(FunctionAnalysis::UseLists)) {
            parameterLocations.grow(function.values.size());
            for (BlockID block : cfg.reversePostOrder()) {
                const auto& parameters = function.at(block).parameters;
                for (uint32_t index = 0; index < parameters.size(); ++index)
                    parameterLocations[parameters[index].index()] = { ValuePlacement::Parameter, block, index };
            }
        }
        ValueLocation parameter = parameterLocations.isEmpty() ? analyses.useLists().locationOf(id) : parameterLocations[id.index()];
        if (parameter.placement != ValuePlacement::Parameter)
            return;
        for (Edge edge : cfg.incomingEdges(parameter.block)) {
            IncomingValue incoming = incomingValueOf(function, edge, parameter.index);
            switch (incoming.kind) {
            case IncomingValue::Kind::Argument:
                mark(incoming.argument);
                break;
            case IncomingValue::Kind::EdgeResult:
                break;
            }
        }
    }

    void activateObjects(PointsToSet pointsTo)
    {
        for (ObjectID object : pointsTo.objects) {
            if (isActive.quickSet(object.index()))
                continue;
            for (ValueID store : storesInto[object.index()])
                mark(store);
        }
    }

    BitVector findDeadEntries(const auto& list) const
    {
        BitVector deadEntries;
        for (size_t index = 0; index < list.size(); ++index) {
            if (!live.get(list[index].index()))
                deadEntries.ensureSizeAndSet(index, list.size());
        }
        return deadEntries;
    }

    static bool isEmptyBlockThatJumps(const Block& block)
    {
        return block.values.isEmpty() && block.parameters.isEmpty() && block.terminator.kind == TerminatorKind::Jump;
    }

    // What dies in the two arms of a test may leave them the same code.
    bool isArmOfTestWithSimilarArms(BlockID id) const
    {
        std::span<const Edge> edges = cfg.incomingEdges(id);
        if (edges.size() != 1)
            return false;
        const Terminator& test = function.at(edges[0].from).terminator;
        if (test.kind != TerminatorKind::Branch)
            return false;
        const Terminator& taken = function.at(test.taken().block).terminator;
        const Terminator& notTaken = function.at(test.notTaken().block).terminator;
        return taken.kind == notTaken.kind && std::ranges::equal(taken.targets, notTaken.targets, { }, &Target::block, &Target::block);
    }

    bool sweep()
    {
        // Whatever is dead at the end was left behind as dead by the walk, or found dead by an edge.
        bool hasDeadValue = false;
        for (size_t wordIndex = 0; !hasDeadValue && wordIndex < live.words.size(); ++wordIndex)
            hasDeadValue = needsAttentionWhenMarked.words[wordIndex] & ~live.words[wordIndex];
        if (!hasDeadValue)
            return false;

        IRBuilder builder(analyses.context(), function, context.types, &analyses);
        bool changed = false;
        for (BlockID id : cfg.reversePostOrder()) {
            const Block& block = function.at(id);
            BitVector deadValues = findDeadEntries(block.values);
            if (!deadValues.isEmpty())
                builder.eraseValues(id, deadValues);
            BitVector deadParameters = findDeadEntries(block.parameters);
            if (!deadParameters.isEmpty()) {
                std::span<const Edge> edges = cfg.incomingEdges(id);
                for (size_t index = 0; index < edges.size(); ++index) {
                    if (edges[index].isHandlerEdge())
                        continue;
                    tookProofsFromEdges |= !!numberOfEdgeResults(function, edges[index].from, edges[index].targetIndex);
                    madeEdgesAlikeOrBlocksEmpty |= index && edges[index - 1].from == edges[index].from;
                    tookArgumentsFromEmptyBlocks |= isEmptyBlockThatJumps(function.at(edges[index].from));
                    madeEdgesAlikeOrBlocksEmpty |= isArmOfTestWithSimilarArms(edges[index].from);
                }
                builder.removeParameters(id, deadParameters);
            }
            if (deadValues.isEmpty() && deadParameters.isEmpty())
                continue;
            changed = true;
            madeEdgesAlikeOrBlocksEmpty |= isEmptyBlockThatJumps(block) || isArmOfTestWithSimilarArms(id);
        }
        RELEASE_ASSERT(changed);
        return true;
    }
};

} // anonymous namespace

PassOutcome eliminateDeadCode(FunctionPassContext& context)
{
    if (context.build.recordedOptions().sabotage) [[unlikely]]
        stopWhereSabotaged(context.build, context.program, context.function, PassID::EliminateDeadCode);
    bool movedKeptCallees = KeptCalleeMover { context }.run();
    DeadCodeEliminator eliminator { context };
    eliminator.walkBackward();
    // A store into an object that does not escape is live once a live value may be the object.
    bool usedEscapeAnalysis = eliminator.findObjectsOfWaitingStores();
    if (usedEscapeAnalysis)
        eliminator.propagate();
    if (!eliminator.sweep())
        return movedKeptCallees ? PassOutcome::Changed : PassOutcome::Unchanged;

    context.keptAnalyses = allAnalyses;
    // With fewer objects left, the escape analysis may prove more on a second run.
    // isCalleeQueried() looks at the function before the sweep, which may remove the call or the last exit that asks for a kept callee.
    bool hasKeptCallees = eliminator.callsWithQueriedCallee.has_value();
    context.reachedFixpoint = !usedEscapeAnalysis && !hasKeptCallees;
    bool mayHaveGivenControlFlowWork = eliminator.madeEdgesAlikeOrBlocksEmpty || eliminator.tookArgumentsFromEmptyBlocks || eliminator.cfg.hasHandlers();
    context.passesLeftIdle = { };
    if (!eliminator.tookProofsFromEdges) {
        context.passesLeftIdle.add(PassID::PropagateBranchConditions);
        if (!context.foldingAskedForUses)
            context.passesLeftIdle.add(PassID::FoldConstants);
        if (!mayHaveGivenControlFlowWork)
            context.passesLeftIdle.add(PassID::SimplifyControlFlow);
    }
    if (!mayHaveGivenControlFlowWork)
        context.passesThatCanWait.add(PassID::SimplifyControlFlow);
    return PassOutcome::Changed;
}

// FoldConstants.

AOT_THRESHOLD(uint64_t, maxPrimitiveTextBytes, 24);
AOT_THRESHOLD(uint64_t, foldedStringBytesPerValue, 64);
AOT_THRESHOLD(uint64_t, maxFullLengthFoldedStringsPerBody, 16);
AOT_THRESHOLD(unsigned, maxOperationsBetweenTextAndPattern, 8);

static constexpr std::pair<ASCIILiteral, TypeofType> namesOfTypeofTypes[] = {
    { "undefined"_s, TypeofType::Undefined },
    { "bigint"_s, TypeofType::BigInt },
    { "boolean"_s, TypeofType::Boolean },
    { "number"_s, TypeofType::Number },
    { "string"_s, TypeofType::String },
    { "symbol"_s, TypeofType::Symbol },
    { "object"_s, TypeofType::Object },
    { "function"_s, TypeofType::Function },
};

static std::optional<TypeofType> typeofTypeNamed(std::span<const uint8_t> text)
{
    for (auto& [name, type] : namesOfTypeofTypes) {
        if (equalSpans(name.span8(), text))
            return type;
    }
    return std::nullopt;
}

static std::optional<Type> typeTestedByTypeOf(const Program& program, TypeofType result, const Type& source)
{
    switch (result) {
    case TypeofType::Undefined:
        return Type::fromPrimitives(Primitive::Undefined);
    case TypeofType::BigInt:
        return Type::fromPrimitives(Primitive::BigInt);
    case TypeofType::Boolean:
        return Type::fromPrimitives(Primitive::Boolean);
    case TypeofType::Number:
        return Type::fromPrimitives(Primitive::Number);
    case TypeofType::String:
        return Type::fromPrimitives(Primitive::String);
    case TypeofType::Symbol:
        return Type::fromPrimitives(Primitive::Symbol);
    case TypeofType::Function:
        return Type::function();
    case TypeofType::Object:
        break;
    }

    // Type::object() includes the callable objects, whose typeof is "function", and no type stands for the others.
    if (!source.overlaps(Type::function(), program))
        return Type::object().withPrimitives(Primitive::Null);
    if (source.withOnlyObjects().isSubsetOf(Type::function(), program))
        return Type::fromPrimitives(Primitive::Null);
    return std::nullopt;
}

namespace {

enum class FoldingRule : uint8_t {
    KnownOperands = 1 << 0,
    SingleValueType = 1 << 1,
    SingleValueBehindCasts = 1 << 2,
    KnownString = 1 << 3,
    OwnRule = 1 << 4,
    MayHaveSeveralResults = 1 << 5,
    MayPassProof = 1 << 6,
};

using FoldingRules = OptionSet<FoldingRule>;

constexpr std::array<FoldingRules, numberOfOpcodes> foldingRulesOfOpcodes = [] {
    std::array<FoldingRules, numberOfOpcodes> result { };
    for (Opcode opcode : allMembersOf<Opcode>) {
        FoldingRules& rules = result[static_cast<unsigned>(opcode)];
        if (!foldableOpcodes.contains(opcode))
            continue;
        if (hasKnownValueRuleOf(opcode))
            rules.add(FoldingRule::KnownOperands);
        if (isCastOrConversion(opcode))
            rules.add(FoldingRule::SingleValueBehindCasts);
        else if (resultRuleOf(opcode) != ResultRule::None && resultRuleOf(opcode) != ResultRule::Allocation && opcode != Opcode::NewClass)
            rules.add(FoldingRule::SingleValueType);
        if (opcodesCreatingKnownStrings.contains(opcode))
            rules.add(FoldingRule::KnownString);
        if (hasOwnFoldingRuleOf(opcode))
            rules.add(FoldingRule::OwnRule);
        if (rules.isEmpty())
            continue;
        if (opcodesThatMayHaveSeveralResults.contains(opcode))
            rules.add(FoldingRule::MayHaveSeveralResults);
        if (provesOf(opcode) != Proof::None)
            rules.add(FoldingRule::MayPassProof);
    }
    return result;
}();

struct ConstantFolder {
    FunctionPassContext& context;
    const Program& program { context.program };
    Function& function { context.function };
    std::optional<BitVector> conditionsWithProofs { };
    // Few bodies ask who uses a value: the users of a text that is too long to fold, which come later in the walk than the text, and the users of a RegExp literal.
    const UseLists* useListsOnceAsked { context.analyses.computed().contains(FunctionAnalysis::UseLists) ? &context.analyses.useLists() : nullptr };
    bool askedForUses { false };

    const UseLists& useLists()
    {
        askedForUses = true;
        if (!useListsOnceAsked)
            useListsOnceAsked = &context.analyses.useLists();
        return *useListsOnceAsked;
    }

    const Type& typeOf(ValueID id) const { return program.typeOf(function, id); }
    const Value& definitionOf(ValueID id) const { return function.at(definitionThroughUpcasts(function, id)); }
    KnownValue knownValueOf(ValueID id) const { return Typed::knownValueOf(program, function, id); }
    static bool isPrimitive(KnownValue value) { return value.isKnown() && !value.isObject(); }

    // A text whose characters are known and that the program makes all the same: a literal would be held in another way.
    struct TextMadeByProgram {
        ValueID value;
        StringID characters;
    };
    Vector<TextMadeByProgram, 4> textsMadeByProgram { };

    KnownValue charactersOfTextMadeByProgram(ValueID id) const
    {
        if (textsMadeByProgram.isEmpty())
            return { };
        RootThroughCasts made = sameObjectThroughCasts(function, id);
        for (const TextMadeByProgram& text : textsMadeByProgram) {
            if (made == text.value)
                return KnownValue::string(text.characters);
        }
        return { };
    }

    // For whoever asks for the characters alone.
    KnownValue knownCharactersOf(ValueID id) const
    {
        KnownValue known = knownValueOf(id);
        return known.isKnown() ? known : charactersOfTextMadeByProgram(id);
    }

    // Its answer is known, so the call ends without an error, and nobody sees what else it leaves behind.
    void takeOutCallsNobodyAsks(Vector<ValueID, 16>& foldedValues)
    {
        const UseLists& uses = useLists();
        for (const TextMadeByProgram& text : textsMadeByProgram) {
            if (function.at(text.value).opcode != Opcode::CallBuiltin || !uses.usesOf(text.value).empty())
                continue;
            explainOperationThatGoes(function, text.value, InPlaceOfOperation::NothingByProofOfThePass);
            becomeUndefined(function, text.value);
            foldedValues.append(text.value);
        }
    }

    bool hasOperandMadeByProgram(const Value& value) const
    {
        if (textsMadeByProgram.isEmpty())
            return false;
        for (uint32_t index = 0; index < value.numberOfOperands(); ++index) {
            ValueID operand = value.operand(index);
            if (operand && !knownValueOf(operand).isKnown() && charactersOfTextMadeByProgram(operand).isKnown())
                return true;
        }
        return false;
    }

    bool becomeConstant(ValueID id, KnownValue constant)
    {
        if (evaluateTypeTest(program, constant, typeOf(id)) != KnownBoolean::True)
            return false;
        Typed::becomeConstant(program, function, context.types, id, constant);
        return true;
    }

    bool becomeBoolean(ValueID id, KnownBoolean result)
    {
        switch (result) {
        case KnownBoolean::Unknown:
            return false;
        case KnownBoolean::False:
            return becomeConstant(id, KnownValue::boolean(false));
        case KnownBoolean::True:
            return becomeConstant(id, KnownValue::boolean(true));
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    bool becomeConstantIfRemovable(ValueID id, KnownValue result)
    {
        return isPrimitive(result) && context.analyses.isRemovableWhenUnused(id) && becomeConstant(id, result);
    }

    bool becomeOperand(ValueID id, const Value& value)
    {
        becomeUpcast(function, id, value.valueOperand());
        return true;
    }

    bool becomeTypeTest(ValueID id, ValueID tested, const Type& type)
    {
        Value& test = function.edit(id);
        test.become(Opcode::TypeTest);
        test.setValueOperand(tested);
        test.setTestedType(context.types.intern(type));
        return true;
    }

    bool foldOnce(ValueID id)
    {
        const Value& value = function.at(id);
        FoldingRules rules = foldingRulesOfOpcodes[static_cast<unsigned>(value.opcode)];
        if (rules.isEmpty())
            return false;
        if (stringJoins.contains(value.opcode)) {
            switch (letJoinWait(id, value)) {
            case KnownStringJoin::Waits:
                return false;
            case KnownStringJoin::IsExpanded:
                return true;
            case KnownStringJoin::IsFoldedAsAnyOperation:
                break;
            }
        }
        if (rules.contains(FoldingRule::MayHaveSeveralResults) && numberOfResultsOf(program, function, value))
            return false;
        if (rules.contains(FoldingRule::MayPassProof)) {
            if (!conditionsWithProofs)
                conditionsWithProofs = conditionsOfBranchesWithProofs(function, context.analyses.cfg());
            if (conditionsWithProofs->get(id.index()))
                return false;
        }
        bool takesTextMadeByProgram = hasOperandMadeByProgram(value);
        // A conversion of a text hands on its operand.
        if (rules.contains(FoldingRule::KnownString) && value.opcode != Opcode::ToString && (takesTextMadeByProgram || joinsNarrowAndWideParts(value))) {
            if (KnownValue joined = joinKnownStrings(id, value); joined.kind() == KnownValue::Kind::String)
                textsMadeByProgram.append({ id, joined.asString() });
            return false;
        }
        if (rules.contains(FoldingRule::KnownOperands)) {
            KnownValue result = evaluateWithKnownOperands(program, function, value, [&](ValueID operand) { return knownCharactersOf(operand); });
            bool isOperandMadeByProgram = takesTextMadeByProgram && result.kind() == KnownValue::Kind::String;
            if (isPrimitive(result) && !isOperandMadeByProgram && becomeConstant(id, result))
                return true;
        }
        if (rules.contains(FoldingRule::SingleValueType)) {
            const Type& type = program.types.at(value.type);
            static_assert(std::has_single_bit(Primitive::Undefined) && std::has_single_bit(Primitive::Null) && std::has_single_bit(Primitive::False) && std::has_single_bit(Primitive::True) && std::has_single_bit(Primitive::Integer) && std::has_single_bit(Primitive::LiteralString));
            if (std::has_single_bit(type.primitives) && becomeConstantIfRemovable(id, knownValueOfType(type)))
                return true;
        } else if (rules.contains(FoldingRule::SingleValueBehindCasts) && resultRuleOf(value.opcode) != ResultRule::None && becomeConstantIfRemovable(id, knownValueOf(id)))
            return true;
        if (rules.contains(FoldingRule::KnownString) && !takesTextMadeByProgram && foldKnownString(id, value))
            return true;
        if (!rules.contains(FoldingRule::OwnRule))
            return false;

        switch (value.opcode) {
        case Opcode::TypeTest:
        case Opcode::IsClosureOf:
            return becomeBoolean(id, evaluateTestByTypes(program, function, value));
        case Opcode::StrictEq:
        case Opcode::Eq:
            return becomeBoolean(id, evaluateTestByTypes(program, function, value)) || foldComparisonOfOneClosure(id, value) || foldTypeOfComparison(id, value);
        case Opcode::TypeOf:
            return foldTypeOf(id, value);
        case Opcode::GetStringCodeUnit:
            return foldCodeUnitOfConstant(id, value);
        case Opcode::ToBoolean:
            return typeOf(value.valueOperand()).isBoolean() ? becomeOperand(id, value) : becomeBoolean(id, evaluateTestByTypes(program, function, value));
        case Opcode::ToString:
            return typeOf(value.valueOperand()).isAnyString() && becomeOperand(id, value);
        case Opcode::ToPropertyKey: {
            const Type& type = typeOf(value.valueOperand());
            return !type.isNever() && type.hasOnlyPrimitives(Primitive::AnyString | Primitive::AnySymbol) && becomeOperand(id, value);
        }
        case Opcode::ElemGet:
            return foldCharacterOfConstant(id, value) || convertToNamedAccess(id, value);
        case Opcode::ElemSet:
        case Opcode::ElemHas:
        case Opcode::ElemDelete:
        case Opcode::ElemDefine:
            return convertToNamedAccess(id, value);
        case Opcode::SuperGet:
        case Opcode::SuperSet:
            return convertToNamedSuperAccess(id, value);
        case Opcode::CopyDataProperties: {
            static constexpr uint16_t primitivesWithoutProperties = Primitive::UndefinedOrNull | Primitive::Boolean | Primitive::Number | Primitive::AnySymbol | Primitive::BigInt;
            if (!typeOf(value.sourceOperand()).hasOnlyPrimitives(primitivesWithoutProperties))
                return false;
            becomeUndefined(function, id);
            return true;
        }
        case Opcode::CallBuiltin:
            return foldStringCall(id, value);
        case Opcode::Substring:
            return foldSubstringOfKnownString(id, value);
        case Opcode::HasInPrototypeChain:
            return foldWalkFromNewObject(id, value);
        case Opcode::GetInternalField:
            return foldFieldOfNewArrayIterator(id, value);
        case Opcode::IsCollectionIteratorClosedMark: {
            Opcode held = function.at(value.tableOperand()).opcode;
            if (held == Opcode::Null)
                return becomeBoolean(id, KnownBoolean::False);
            return held == Opcode::CollectionIteratorClosedMark && becomeBoolean(id, KnownBoolean::True);
        }
        case Opcode::HasJSType:
            return foldKindOfNewIterator(id, value);
        default:
            return false;
        }
    }

    bool foldKindOfNewIterator(ValueID id, const Value& test)
    {
        const Value& made = function.at(sameObjectThroughCasts(function, test.valueOperand()));
        JSType type;
        if (makesArrayIterator(program, function, made))
            type = JSArrayIteratorType;
        else if (makesCollectionIterator(program, function, made))
            type = program.typeOf(function, made.receiverOperand()).isSubsetOf(Type::builtinClass(BuiltinClasses::Map), program) ? JSMapIteratorType : JSSetIteratorType;
        else
            return false;
        return becomeBoolean(id, knownBoolean(toJSType(test.first()) <= type && type <= toJSType(test.last())));
    }

    // `const it = set.values(); it.next()`: a Set or Map iterator gets its collection and its kind when it is made, and nothing stores to either again.
    bool foldFieldOfNewCollectionIterator(ValueID id, const Value& read, const Value& made)
    {
        BuiltinID builtin = program.builtins().at(made.overload()).builtin;
        IterationKind kind = builtin == Builtins::MapPrototypeKeys ? IterationKind::Keys : builtin == Builtins::MapPrototypeValues || builtin == Builtins::SetPrototypeValues ? IterationKind::Values : IterationKind::Entries;
        switch (static_cast<JSSetIterator::Field>(read.index())) {
        case JSSetIterator::Field::Entry:
        case JSSetIterator::Field::Storage:
            return false;
        case JSSetIterator::Field::IteratedObject:
            becomeUpcast(function, id, made.receiverOperand());
            return true;
        case JSSetIterator::Field::Kind:
            return becomeConstant(id, KnownValue::integer(static_cast<int64_t>(kind)));
        }
        return false;
    }

    // `const it = list[Symbol.iterator](); it.next()`: an array iterator gets its list and its kind when it is made, and nothing stores to either again.
    bool foldFieldOfNewArrayIterator(ValueID id, const Value& read)
    {
        const Value& made = function.at(sameObjectThroughCasts(function, read.objectOperand()));
        if (makesCollectionIterator(program, function, made))
            return foldFieldOfNewCollectionIterator(id, read, made);
        if (!makesArrayIterator(program, function, made))
            return false;
        BuiltinID builtin = program.builtins().at(made.overload()).builtin;
        IterationKind kind = builtin == Builtins::ArrayPrototypeKeys ? IterationKind::Keys : builtin == Builtins::ArrayPrototypeValues ? IterationKind::Values : IterationKind::Entries;
        switch (static_cast<JSArrayIterator::Field>(read.index())) {
        case JSArrayIterator::Field::Index:
            return false;
        case JSArrayIterator::Field::IteratedObject:
            becomeUpcast(function, id, made.receiverOperand());
            return true;
        case JSArrayIterator::Field::Kind:
            return becomeConstant(id, KnownValue::integer(static_cast<int64_t>(kind)));
        }
        return false;
    }

    // `this instanceof F` in the body of F, behind the allocation: an object that is handed no prototype is born with the prototype object of its class, and no object of the layout ever gets another.
    bool foldWalkFromNewObject(ValueID id, const Value& walk)
    {
        const Value& object = function.at(sameObjectThroughCasts(function, walk.valueOperand()));
        const Value& prototype = function.at(sameObjectThroughCasts(function, walk.prototypeOperand()));
        if (object.opcode != Opcode::NewObject || object.prototypeOperand() || prototype.opcode != Opcode::GetClassPrototype)
            return false;
        if (!allocatesOwnLayout(program, object) || declaredLayoutOf(program, object.layout()) != prototype.layout())
            return false;
        return receiverStateOf(program, object.layout()).cannot(ObjectMutation::ChangePrototype) && becomeBoolean(id, KnownBoolean::True);
    }

    // `stream.emit === emit`: the program makes one closure of the function, so whatever is a closure of it is that object.
    bool foldComparisonOfOneClosure(ValueID id, const Value& comparison)
    {
        for (unsigned i = 0; i < 2; ++i) {
            const Type& closure = typeOf(comparison.operand(i));
            FunctionID callee = closure.singleFunction();
            if (!callee || closure.mayBeUndefinedOrNull() || !context.inputs.flow.hasOneClosure(callee))
                continue;
            ValueID other = comparison.operand(1 - i);
            const Type& otherType = typeOf(other);
            if (!otherType.isJavaScriptValue() || otherType.withOnlyObjects().singleFunction() != callee)
                continue;
            if (otherType.singleFunction() == callee && !otherType.mayBeUndefinedOrNull())
                return becomeBoolean(id, KnownBoolean::True);
            return becomeTypeTest(id, other, Type::forFunction(callee));
        }
        return false;
    }

    bool foldTypeOf(ValueID id, const Value& value)
    {
        const Type& source = typeOf(value.valueOperand());
        if (source.isNever())
            return false;
        for (auto& [name, type] : namesOfTypeofTypes) {
            std::optional<Type> tested = typeTestedByTypeOf(program, type, source);
            if (tested && evaluateTypeTest(program, source, *tested) == KnownBoolean::True)
                return becomeConstant(id, KnownValue::string(context.strings.intern(name.span8())));
        }
        return false;
    }

    bool foldCodeUnitOfConstant(ValueID id, const Value& read)
    {
        KnownValue text = knownValueOf(read.stringOperand());
        KnownValue position = knownValueOf(read.positionOperand());
        if (text.kind() != KnownValue::Kind::String || position.kind() != KnownValue::Kind::Integer)
            return false;
        Vector<char16_t> buffer;
        StringView characters = program.strings.viewOf(text.asString(), buffer);
        if (position.asInteger() < 0 || position.asInteger() >= static_cast<int64_t>(characters.length()))
            return false;
        return becomeConstant(id, KnownValue::integer(characters[position.asInteger()]));
    }

    bool foldTypeOfComparison(ValueID id, const Value& value)
    {
        for (unsigned i = 0; i < 2; ++i) {
            const Value& typeOfOperation = definitionOf(value.operand(i));
            const Value& string = definitionOf(value.operand(1 - i));
            if (typeOfOperation.opcode != Opcode::TypeOf || string.opcode != Opcode::String)
                continue;
            auto expectedTypeof = typeofTypeNamed(program.string(string.string()));
            if (!expectedTypeof)
                return becomeConstant(id, KnownValue::boolean(false));
            ValueID operand = typeOfOperation.valueOperand();
            std::optional<Type> tested = typeTestedByTypeOf(program, *expectedTypeof, typeOf(operand));
            return tested && becomeTypeTest(id, operand, *tested);
        }
        return false;
    }

    // A piece of such a text that lies in an 8-bit part is an 8-bit string. A piece of a 16-bit literal is not.
    bool joinsNarrowAndWideParts(const Value& value)
    {
        bool hasNarrowPart = false;
        bool hasWidePart = false;
        for (uint32_t index = 0; index < value.numberOfOperands(); ++index) {
            KnownValue operand = knownValueOf(value.operand(index));
            if (operand.kind() != KnownValue::Kind::String) {
                hasNarrowPart |= isPrimitive(operand);
                continue;
            }
            std::span<const uint8_t> text = program.string(operand.asString());
            // In UTF-8 a larger byte begins a character outside Latin-1.
            bool isWide = std::ranges::any_of(text, [](uint8_t byte) { return byte > 0xc3; });
            hasWidePart |= isWide;
            hasNarrowPart |= !isWide && !text.empty();
        }
        return hasNarrowPart && hasWidePart;
    }

    // `s = s + n + a;` line after line, all constant: each concatenation is only used by the next, and nothing reads its characters. Such a concatenation waits, and the first one with
    // another use gathers the pieces, so that n concatenations leave one string instead of n strings of up to n pieces.
    static constexpr OpcodeSet stringJoins { Opcode::Add, Opcode::Strcat };
    enum class KnownStringJoin : uint8_t { Waits, IsExpanded, IsFoldedAsAnyOperation };
    // Indexed by value: the byte size of its string.
    UncheckedKeyHashMap<uint32_t, uint64_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> waitingJoins { };
    Vector<ValueID, 4> expandedJoins { };

    bool isReadByOnlyOneJoin(ValueID id)
    {
        std::optional<Use> use = useLists().singleUse(id);
        return use && use->kind == Use::Kind::Operand && stringJoins.contains(function.at(use->user).opcode);
    }

    bool expandJoin(ValueID id, uint64_t bytes)
    {
        if (exceedsLimitOfFoldedString(id, bytes))
            return false;
        StringBuilder result(OverflowPolicy::RecordOverflow);
        Vector<ValueID, 16> left { id };
        Vector<ValueID, 16> gathered;
        while (!left.isEmpty()) {
            ValueID next = left.takeLast();
            if (next != id && !waitingJoins.contains(next.index())) {
                result.append(*textOf(program, knownValueOf(next)));
                continue;
            }
            if (next != id)
                gathered.append(next);
            const Value& join = function.at(next);
            for (uint32_t index = join.numberOfOperands(); index--;)
                left.append(join.operand(index));
        }
        if (result.hasOverflowed() || !becomeConstant(id, KnownValue::string(context.strings.intern(StringView(result)))))
            return false;
        context.bytesOfFoldedStrings += bytes;
        waitingJoins.remove(id.index());
        // Each was only read by the concatenation above it, and produced a shorter string than this one.
        for (ValueID join : gathered) {
            waitingJoins.remove(join.index());
            explainOperationThatGoes(function, join, InPlaceOfOperation::NothingByProofOfThePass);
            becomeUndefined(function, join);
            expandedJoins.append(join);
        }
        context.analyses.invalidate(FunctionAnalysis::Effects);
        return true;
    }

    KnownStringJoin letJoinWait(ValueID id, const Value& value)
    {
        // The representation of a string that the program creates, and of a string with narrow and wide parts, is decided by the rules below.
        bool canWait = textsMadeByProgram.isEmpty();
        bool hasWaitingOperand = false;
        bool hasText = false;
        uint64_t bytes = 0;
        uint64_t mostBytes = 0;
        for (uint32_t index = 0; index < value.numberOfOperands(); ++index) {
            ValueID operand = value.operand(index);
            if (auto waiting = operand ? waitingJoins.find(operand.index()) : waitingJoins.end(); waiting != waitingJoins.end()) {
                hasWaitingOperand = true;
                hasText = true;
                bytes += waiting->value;
                mostBytes += waiting->value;
                continue;
            }
            KnownValue known = canWait && operand ? knownValueOf(operand) : KnownValue();
            if (!isPrimitive(known)) {
                canWait = false;
                continue;
            }
            if (known.kind() != KnownValue::Kind::String) {
                bytes += textOf(program, known)->length();
                mostBytes += maxPrimitiveTextBytes;
                continue;
            }
            std::span<const uint8_t> text = program.string(known.asString());
            canWait = !std::ranges::any_of(text, [](uint8_t byte) { return byte > 0xc3; });
            hasText = true;
            bytes += text.size();
            mostBytes += text.size();
        }
        canWait = canWait && hasText && mostBytes <= context.limit(LimitID::MaxFoldedStringBytes);
        if (canWait && isReadByOnlyOneJoin(id)) {
            waitingJoins.add(id.index(), bytes);
            return KnownStringJoin::Waits;
        }
        if (!hasWaitingOperand)
            return KnownStringJoin::IsFoldedAsAnyOperation;
        if (canWait && expandJoin(id, bytes))
            return KnownStringJoin::IsExpanded;
        for (uint32_t index = 0; index < value.numberOfOperands(); ++index) {
            ValueID operand = value.operand(index);
            auto waiting = operand ? waitingJoins.find(operand.index()) : waitingJoins.end();
            if (waiting != waitingJoins.end() && expandJoin(operand, waiting->value))
                expandedJoins.append(operand);
        }
        return KnownStringJoin::IsFoldedAsAnyOperation;
    }

    KnownValue joinKnownStrings(ValueID id, const Value& value)
    {
        if (!opcodesCreatingKnownStrings.contains(value.opcode))
            return { };

        uint64_t bytes = 0;
        for (uint32_t index = 0; index < value.numberOfOperands(); ++index) {
            KnownValue operand = knownCharactersOf(value.operand(index));
            if (!isPrimitive(operand))
                return { };
            bytes += operand.kind() == KnownValue::Kind::String ? program.string(operand.asString()).size() : maxPrimitiveTextBytes;
        }
        if (exceedsLimitOfFoldedString(id, bytes))
            return { };
        KnownValue joined = evaluateStringOperation(program, context.strings, function, value, [&](ValueID operand) { return knownCharactersOf(operand); });
        if (joined.kind() == KnownValue::Kind::String)
            context.bytesOfFoldedStrings += bytes;
        return joined;
    }

    bool foldKnownString(ValueID id, const Value& value)
    {
        KnownValue result = joinKnownStrings(id, value);
        return result.kind() == KnownValue::Kind::String && becomeConstant(id, result);
    }

    const BuiltinOverload* wholeCallOf(const Value& value) const
    {
        if (value.opcode != Opcode::CallBuiltin || value.builtinCallForm() != BuiltinCallForm::Whole)
            return nullptr;
        return &program.builtins().at(value.overload());
    }

    BuiltinID calledStringFunctionOf(const Value& value) const
    {
        const BuiltinOverload* overload = wholeCallOf(value);
        if (!overload)
            return { };
        BuiltinID callee = program.builtins().canonicalID(overload->builtin);
        // What such a function answers depends on its operands and on nothing else: no locale, no clock.
        if (overload->mode != BuiltinCallMode::Call || (!program.builtins().at(callee).flags.contains(BuiltinFlag::IsDeterministic) && !formatsNumberInNamedLocale(callee, value)))
            return { };
        return callee;
    }

    // In a locale that the call names, and without options, the text depends on the number and on the data of ICU.
    bool formatsNumberInNamedLocale(BuiltinID callee, const Value& value) const
    {
        if (callee != Builtins::NumberPrototypeToLocaleString || targetOfThisBuild().platform == BuiltinPlatform::Darwin)
            return false;
        std::span<const ValueID> arguments = value.argumentsOperands();
        if (arguments.empty() || arguments.size() > 2 || knownValueOf(arguments[0]).kind() != KnownValue::Kind::String)
            return false;
        return arguments.size() == 1 || knownValueOf(arguments[1]).kind() == KnownValue::Kind::Undefined;
    }

    bool isPatternOfCreatedRegExp(const Value& user, ValueID text) const
    {
        const BuiltinOverload* overload = wholeCallOf(user);
        if (!overload || (overload->mode != BuiltinCallMode::Call && overload->mode != BuiltinCallMode::Construct) || program.builtins().canonicalID(overload->builtin) != Builtins::RegExp)
            return false;
        std::span<const ValueID> arguments = user.argumentsOperands();
        return !arguments.empty() && arguments[0] == text && std::ranges::count(arguments, text) == 1 && user.receiverOperand() != text;
    }

    // FoldConstantRegExps spells such a text once, as the pattern of the literal that it makes of the call.
    bool feedsOnlyRegExpPatterns(ValueID text, unsigned operationsLeft = maxOperationsBetweenTextAndPattern)
    {
        std::span<const Use> uses = useLists().usesOf(text);
        return !uses.empty() && std::ranges::all_of(uses, [&](const Use& use) {
            if (use.kind != Use::Kind::Operand)
                return false;
            const Value& user = function.at(use.user);
            if (isPatternOfCreatedRegExp(user, text))
                return true;
            bool makesTextOfOperands = isCastThatProvesNothing(user.opcode) || opcodesCreatingKnownStrings.contains(user.opcode) || calledStringFunctionOf(user);
            return makesTextOfOperands && operationsLeft && feedsOnlyRegExpPatterns(use.user, operationsLeft - 1);
        });
    }

    // `s = s + n + a; use(s);` line after line: each string is as long as all the lines before it, and the program keeps every one. So the strings of one body are limited in total to a few times the limit, plus an allowance that grows with the body.
    bool exceedsLimitOfFoldedString(ValueID id, uint64_t bytes)
    {
        if (bytes > context.limit(LimitID::MaxFoldedStringBytes) && feedsOnlyRegExpPatterns(id))
            return context.exceeds(LimitID::RegExpCandidateTextLength, bytes, id);
        uint64_t together = context.bytesOfFoldedStrings + bytes;
        uint64_t shareOfBody = foldedStringBytesPerValue * function.values.size();
        uint64_t beyondShare = together > shareOfBody ? together - shareOfBody : 0;
        return context.exceeds(LimitID::MaxFoldedStringBytes, std::max(bytes, beyondShare / maxFullLengthFoldedStringsPerBody), id);
    }

    // A literal that nothing but the call uses is as created when the call begins, and nobody sees what the call leaves in it.
    std::optional<RegExpLiteralInType> findLiteralUsedOnlyBy(ValueID user, ValueID regExp)
    {
        const UseLists& uses = useLists();
        for (;;) {
            std::optional<Use> use = uses.singleUse(regExp);
            if (!use || use->kind != Use::Kind::Operand || use->user != user)
                return std::nullopt;
            const Value& definition = function.at(regExp);
            if (definition.opcode == Opcode::NewRegExp)
                return RegExpLiteralInType { definition.pattern(), definition.regExpFlags().toRaw() };
            if (!isCastThatProvesNothing(definition.opcode) && definition.opcode != Opcode::AssumeType)
                return std::nullopt;
            user = regExp;
            regExp = definition.valueOperand();
        }
    }

    // The literal, and the type of the operand when the call begins.
    std::optional<std::pair<RegExpLiteralInType, TypeID>> regExpOperandOf(ValueID call, ValueID operand)
    {
        std::optional<RegExpLiteralInType> literal = findLiteralUsedOnlyBy(call, operand);
        // Only built-ins that leave the object as created get a shared literal, and the call is one of them.
        const Value& definition = function.at(sameObjectThroughCasts(function, operand));
        if (!literal && definition.opcode == Opcode::SharedRegExpLiteral)
            literal = RegExpLiteralInType { definition.pattern(), definition.regExpFlags().toRaw() };
        if (literal) {
            Value shared(Opcode::SharedRegExpLiteral);
            shared.setPattern(literal->pattern);
            shared.setRegExpFlags(literal->flags());
            return std::pair { *literal, resultTypeOf(program, function, shared, context.types) };
        }
        // A match without the flags g and y neither depends on lastIndex nor stores it.
        literal = singleRegExpLiteralAsCreatedOf(typeOf(operand));
        if (!literal || literal->flags().containsAny({ RegExpFlags::Global, RegExpFlags::Sticky }))
            return std::nullopt;
        return std::pair { *literal, function.at(operand).type };
    }

    std::optional<int64_t> knownInteger(ValueID id) const
    {
        KnownValue known = knownValueOf(id);
        if (!known.isNumber() || known.asNumber() != std::trunc(known.asNumber()) || std::abs(known.asNumber()) > INT32_MAX)
            return std::nullopt;
        return static_cast<int64_t>(known.asNumber());
    }

    // Where a bound of a part of a text with `length` code units comes out of StringRange, as `s.slice(a, b)` and `s.substring(a, b)` have it.
    std::optional<int64_t> knownBoundOfPart(ValueID id, int64_t length) const
    {
        if (std::optional<int64_t> integer = knownInteger(id))
            return integer;
        const Value& result = function.at(id);
        if (result.opcode != Opcode::ResultAt || function.at(result.resultsOperand()).opcode != Opcode::StringRange)
            return std::nullopt;
        const Value& range = function.at(result.resultsOperand());
        if (knownInteger(range.lengthOperand()) != length || (range.method() != StringRangeMethod::Slice && range.method() != StringRangeMethod::Substring))
            return std::nullopt;
        std::optional<int64_t> first = knownInteger(range.firstOperand());
        std::optional<int64_t> second = range.secondOperand() ? knownInteger(range.secondOperand()) : length;
        if (!first || !second)
            return std::nullopt;
        int64_t start;
        int64_t end;
        if (range.method() == StringRangeMethod::Slice) {
            auto relative = [&](int64_t index) { return index < 0 ? std::max<int64_t>(length + index, 0) : std::min(index, length); };
            start = relative(*first);
            end = std::max(start, relative(*second));
        } else {
            start = std::clamp<int64_t>(std::min(*first, *second), 0, length);
            end = std::clamp<int64_t>(std::max(*first, *second), 0, length);
        }
        return result.index() ? end : start;
    }

    bool foldSubstringOfKnownString(ValueID id, const Value& value)
    {
        KnownValue known = knownValueOf(value.stringOperand());
        if (known.kind() != KnownValue::Kind::String)
            return false;
        String text = program.strings.toWTFString(known.asString());
        // A part of a wide text may be half of a pair.
        if (!text.is8Bit())
            return false;
        int64_t length = text.length();
        std::optional<int64_t> start = knownBoundOfPart(value.startOperand(), length);
        std::optional<int64_t> end = knownBoundOfPart(value.endOperand(), length);
        if (!start || !end || *start < 0 || *end > length || *start > *end)
            return false;
        return becomeConstant(id, KnownValue::string(context.strings.intern(StringView(text).substring(*start, *end - *start))));
    }

    bool foldStringCall(ValueID id, const Value& value)
    {
        BuiltinID callee = calledStringFunctionOf(value);
        if (!callee)
            return false;
        ValueID receiverOperand = value.receiverOperand();
        KnownValue receiver = receiverOperand ? knownCharactersOf(receiverOperand) : KnownValue();
        bool takesNumber = callee == Builtins::NumberPrototypeToLocaleString;
        if (takesNumber ? !receiver.isNumber() : receiver.kind() != KnownValue::Kind::String)
            return false;

        uint64_t bytes = takesNumber ? maxPrimitiveTextBytes : program.string(receiver.asString()).size();
        double largestNumber = 1;
        Vector<ScratchRealm::Operand, 2> arguments;
        Vector<TypeID, 2> argumentTypes;
        for (ValueID argument : value.argumentsOperands()) {
            KnownValue primitive = knownCharactersOf(argument);
            if (isPrimitive(primitive)) {
                if (primitive.kind() == KnownValue::Kind::String)
                    bytes += program.string(primitive.asString()).size();
                if (primitive.isNumber() && primitive.asNumber() > largestNumber)
                    largestNumber = primitive.asNumber();
                arguments.append(ScratchRealm::Operand { primitive, std::nullopt });
                argumentTypes.append(function.at(argument).type);
                continue;
            }
            auto regExp = regExpOperandOf(id, argument);
            if (!regExp)
                return false;
            bytes += program.string(regExp->first.pattern).size();
            arguments.append(ScratchRealm::Operand { { }, regExp->first });
            argumentTypes.append(regExp->second);
        }

        EffectDescription description;
        describeOverload(program, value.overload(), function.at(receiverOperand).type, argumentTypes.span(), description);
        if (description.callsUnknownCode || !description.functions.isEmpty())
            return false;
        static constexpr AbstractHeapSet heapsNobodyObserves = bookkeepingHeaps | AbstractHeapSet { AbstractHeap::StringStorage };
        Effects effects = description.own.onAnyEdge();
        if (effects.mayRunOtherCode() || !effects.writes.containsOnly(heapsNobodyObserves | AbstractHeapSet { AbstractHeap::RegExpState }))
            return false;
        // The RegExp of the call is left as it was or is seen by nobody, so what the last match leaves in the constructor is the one trace of a match.
        if (!effects.writes.containsOnly(heapsNobodyObserves) && context.inputs.flow.mayReadLegacyRegExpStatics())
            return false;
        if (exceedsLimitOfFoldedString(id, bytes))
            return false;
        // An entry with an error of its own may fail for the length of its answer, which a number among its operands sets: a count, a length.
        bool pads = callee == Builtins::StringPrototypePadStart || callee == Builtins::StringPrototypePadEnd;
        if (pads || !program.builtins().at(value.overload()).onlyPropagatesErrors()) {
            double bytesOfAnswer = static_cast<double>(bytes) * largestNumber;
            if (bytesOfAnswer >= static_cast<double>(std::numeric_limits<uint32_t>::max()) || exceedsLimitOfFoldedString(id, static_cast<uint64_t>(bytesOfAnswer)))
                return false;
        }

        std::optional<ScratchRealm::Answer> answer = context.build.scratchRealms().ofCurrentThread().call(program, callee, receiver, arguments.span());
        if (!answer)
            return false;
        auto becomeAnswer = [&](KnownValue known) {
            explainOperationThatGoes(function, id, InPlaceOfOperation::NothingByProofOfThePass);
            return becomeConstant(id, known);
        };
        if (answer->text.isNull())
            return becomeAnswer(answer->primitive);
        if (exceedsLimitOfFoldedString(id, answer->text.sizeInBytes()))
            return false;
        context.bytesOfFoldedStrings += answer->text.sizeInBytes();
        StringID characters = context.strings.intern(StringView(answer->text));
        // How the answer is held follows how the operands are, and the realm was handed literals.
        if (answer->isStoredLikeLiteral && !hasOperandMadeByProgram(value))
            return becomeAnswer(KnownValue::string(characters));
        textsMadeByProgram.append({ id, characters });
        return false;
    }

    PropertyKeyID findNameGivenTo(const Value& operation, ValueID key)
    {
        return internKey(context.types, constantNameOf(program, program.sourceFunctionOf(function, operation), typeOf(key)));
    }

    bool convertToNamedSuperAccess(ValueID id, const Value& value)
    {
        ValueID computedKey = value.computedKeyOperand();
        PropertyKeyID key = computedKey ? findNameGivenTo(value, computedKey) : PropertyKeyID();
        if (!key)
            return false;
        Value& access = function.edit(id, ValueEdit::Definition);
        access.setComputedKeyOperand({ });
        access.setKey(key);
        return true;
    }

    // `'hello'[0]`: the characters of a string are its own properties, and nothing can change them. Past the end the read asks String.prototype, so it stays.
    bool foldCharacterOfConstant(ValueID id, const Value& read)
    {
        KnownValue text = knownValueOf(read.objectOperand());
        KnownValue index = knownValueOf(read.propertyOperand());
        if (text.kind() != KnownValue::Kind::String || index.kind() != KnownValue::Kind::Integer)
            return false;
        Vector<char16_t> buffer;
        StringView characters = program.strings.viewOf(text.asString(), buffer);
        if (index.asInteger() < 0 || index.asInteger() >= static_cast<int64_t>(characters.length()))
            return false;
        explainOperationThatGoes(function, id, InPlaceOfOperation::NothingByProofOfThePass);
        return becomeConstant(id, KnownValue::string(context.strings.intern(characters.substring(index.asInteger(), 1))));
    }

    bool convertToNamedAccess(ValueID id, const Value& value)
    {
        PropertyKeyID key = findNameGivenTo(value, value.propertyOperand());
        if (!key)
            return false;

        TypeArena& types = context.types;
        TypeID expected = value.expectedOrType().predicted();
        Value& access = function.edit(id);
        becomeNamedAccess(access, key);
        access.type = resultTypeOf(program, function, access, types);
        TypeID narrowed = types.intersectionOf(access.type, expected);
        // An intersection may keep a part of one side that the subset test does not find in the other.
        bool saysMoreThanType = narrowed != TypeTable::never && narrowed != access.type && types.at(narrowed).isSubsetOf(types.at(access.type), program);
        access.setExpected(ExpectedType(saysMoreThanType ? narrowed : TypeID()));
        if (access.opcode == Opcode::PropGet)
            nameCalleeOfMemberCalls(id, key);
        return true;
    }

    // A call of what was read under a name, on the object that it was read from, is a call of the member of that name.
    void nameCalleeOfMemberCalls(ValueID read, PropertyKeyID key)
    {
        ValueID object = definitionThroughSameValues(function, function.at(read).objectOperand());
        Vector<ValueID, 4> worklist { read };
        while (!worklist.isEmpty()) {
            ValueID callee = worklist.takeLast();
            for (const Use& use : useLists().usesOf(callee)) {
                if (use.kind != Use::Kind::Operand)
                    continue;
                const Value& user = function.at(use.user);
                if (sameValueOperandOf(user) == callee)
                    worklist.append(use.user);
                else if (user.opcode == Opcode::Call && use.operandIndex == OperandIndex::Call::callee && !user.calleeKey() && user.thisValueOperand() && definitionThroughSameValues(function, user.thisValueOperand()) == object)
                    function.edit(use.user).setCalleeKey(key);
            }
        }
    }
};

} // anonymous namespace

PassOutcome foldConstants(FunctionPassContext& context)
{
    ConstantFolder folder { context };
    Function& function = context.function;
    Vector<ValueID, 16> foldedValues;
    bool didChangeCasts = false;
    for (BlockID blockID : context.analyses.cfg().reversePostOrder()) {
        const Block& block = function.at(blockID);
        ValueID tailCall = block.terminator.kind == TerminatorKind::ReturnCall ? block.terminator.callOperand() : ValueID();
        for (ValueID id : block.values) {
            Opcode opcode = function.at(id).opcode;
            if (foldingRulesOfOpcodes[static_cast<unsigned>(opcode)].isEmpty() || id == tailCall || !folder.foldOnce(id))
                continue;
            // The walk goes on to ask what this value and its users do.
            do {
                context.analyses.invalidate(FunctionAnalysis::Effects);
            } while (folder.foldOnce(id));
            foldedValues.append(id);
            didChangeCasts |= isCastOrConversion(opcode) || isCastOrConversion(function.at(id).opcode);
        }
    }
    foldedValues.appendVector(folder.expandedJoins);
    if (!folder.textsMadeByProgram.isEmpty()) {
        if (!foldedValues.isEmpty()) {
            context.analyses.invalidate(FunctionAnalysis::UseLists);
            folder.useListsOnceAsked = nullptr;
        }
        folder.takeOutCallsNobodyAsks(foldedValues);
    }
    context.foldingAskedForUses = folder.askedForUses;
    if (foldedValues.isEmpty())
        return PassOutcome::Unchanged;

    // Which values are one value is read from the casts, from what the edges pass and from the tests whose branches pass proofs. The last two stay as they are.
    AnalysisSet unchangedAnalyses;
    unchangedAnalyses.set(FunctionAnalysis::Identities, !didChangeCasts);
    context.analyses.invalidate(valueAnalyses - unchangedAnalyses);
    narrowValueTypes(TypePropagationInputs { context.program, context.id, function, context.analyses, context.types, context.build, &context.typingMemo }, foldedValues.span());
    context.keptAnalyses = allAnalyses;
    return PassOutcome::Changed;
}

// RemoveTrivialParameters.

static bool canReplaceParameter(const Function& function, ValueID argument, ValueID parameter)
{
    const Value& replacement = function.at(argument);
    const Value& replaced = function.at(parameter);
    return replacement.type == replaced.type && replacement.expected().predicted() == replaced.expected().predicted();
}

static bool isCastOf(const Function& function, const ValueSubstitution& substitution, ValueID value, ValueID of)
{
    while (isCast(function.at(value).opcode)) {
        value = substitution.resolve(function.at(value).operand(0));
        if (value == of)
            return true;
    }
    return false;
}

namespace {

struct ParameterPosition {
    ValueID parameter;
    BlockID block;
    uint32_t index { 0 };
};

struct TrivialParameterRemover {
    static constexpr uint32_t noPosition = UINT32_MAX;

    FunctionPassContext& context;
    Vector<ParameterPosition> parameters;
    Function& function { context.function };
    FunctionAnalyses& analyses { context.analyses };
    const CFG& cfg { analyses.cfg() };
    Vector<ValueID> leaders = Vector<ValueID>(function.values.size(), [](size_t index) { return ValueID(index); });
    ValueSubstitution candidates { function };
    Vector<uint32_t> positionOfParameter = Vector<uint32_t>(FillWith { }, function.values.size(), noPosition);

    mutable std::optional<BitVector> positionsFoundInBounds;

    bool isNarrowedOnEdge(ValueID parameter) const
    {
        uint32_t index = positionOfParameter[parameter.index()];
        if (index == noPosition)
            return false;
        const ParameterPosition& position = parameters[index];
        return std::ranges::any_of(cfg.incomingEdges(position.block), [&](Edge edge) {
            return position.index >= targetOf(function, edge).arguments.size();
        });
    }

    bool isPositionThatTestFoundInBounds(ValueID parameter) const
    {
        if (!positionsFoundInBounds) {
            positionsFoundInBounds.emplace(function.values.size());
            for (BlockID block : cfg.reversePostOrder()) {
                for (ValueID id : function.at(block).values) {
                    const Value& value = function.at(id);
                    if (provesOf(value.opcode) == Proof::InBounds && value.positionOperand())
                        positionsFoundInBounds->quickSet(value.positionOperand().index());
                }
            }
        }
        return positionsFoundInBounds->quickGet(parameter.index());
    }

    // `index < length` says nothing of the index while the declared type of the length is only expected. It does behind the test of the length, or where the body stands in a caller that knows the length.
    bool isLeftByComparisonWithExpectedNumber(const ParameterPosition& position) const
    {
        std::optional<Edge> edge = cfg.singleIncomingEdge(position.block);
        if (!edge || edge->isHandlerEdge() || !function.at(edge->from).terminator.isConditionalBranch())
            return false;
        const Value& comparison = function.at(function.at(edge->from).terminator.conditionOperand());
        uint32_t first = firstEdgeResultParameter(function, edge->from, edge->targetIndex);
        if (!relationalOperators.contains(comparison.opcode) || position.index < first || !context.program.sourceFunctionOf(function, comparison.origin()).has(FunctionFlags::IsChecked))
            return false;
        uint32_t narrowedOperandIndex = targetOf(function, *edge).proofs[position.index - first];
        const Value& other = function.at(comparison.operand(1 - narrowedOperandIndex));
        return context.types.at(function.at(position.parameter).type).isNumber() && !context.types.at(other.type).isNumber() && context.types.at(other.expectedOrType().predicted()).isNumber();
    }

    KnownValue onlyValueOf(const Block& block, ValueID parameter) const
    {
        if (block.handler || block.subject == parameter)
            return { };
        KnownValue value = knownValueOfType(context.types.at(function.at(parameter).type));
        switch (value.kind()) {
        case KnownValue::Kind::Undefined:
        case KnownValue::Kind::Null:
        case KnownValue::Kind::Boolean:
            return value;
        case KnownValue::Kind::Integer:
            // What a test has found in bounds is the value that was narrowed to the parameter. A constant in its place says nothing of that value.
            return isNarrowedOnEdge(parameter) && isPositionThatTestFoundInBounds(parameter) ? KnownValue() : value;
        case KnownValue::Kind::Unknown:
        case KnownValue::Kind::Double:
        case KnownValue::Kind::String:
        case KnownValue::Kind::Object:
        case KnownValue::Kind::Function:
            return { };
        }
    }

    ValueID leaderOf(ValueID value) const { return leaders[candidates.resolve(value).index()]; }

    ValueID computeLeader(const ParameterPosition& position) const
    {
        ValueID only;
        bool receivesEdgeResult = false;
        for (Edge edge : cfg.incomingEdges(position.block)) {
            const Arguments& arguments = targetOf(function, edge).arguments;
            bool isExplicitArgument = position.index < arguments.size();
            ValueID passed = isExplicitArgument ? arguments[position.index] : valuePassedOn(function, edge, position.index);
            if (!passed)
                return position.parameter;
            receivesEdgeResult |= !isExplicitArgument;

            ValueID leader = leaderOf(passed);
            if (!leader || leader == position.parameter || leader == only || isCastOf(function, candidates, leader, position.parameter))
                continue;
            if (only)
                return valueBehindCasts(position);
            only = leader;
        }
        if (receivesEdgeResult && only && (!canReplaceParameter(function, only, position.parameter) || isLeftByComparisonWithExpectedNumber(position)))
            return position.parameter;
        return only;
    }

    // The edges pass one value, some of them under a stronger type from a cast or a test, and the join has forgotten what those proved.
    ValueID valueBehindCasts(const ParameterPosition& position) const
    {
        ValueID value = analyses.identityOf(position.parameter);
        if (value == position.parameter)
            return value;
        const Type& joined = context.types.at(function.at(position.parameter).type);
        auto fitsJoin = [&](ValueID candidate) { return context.types.at(function.at(candidate).type).isSubsetOf(joined, context.program); };
        if (fitsJoin(value))
            return value;
        ValueID narrowed = findNarrowedValueOfEveryEdge(position);
        return narrowed && fitsJoin(narrowed) ? narrowed : position.parameter;
    }

    ValueID sourceOfNarrowedValue(ValueID value) const
    {
        const Value& definition = function.at(value);
        if (isCast(definition.opcode))
            return candidates.resolve(definition.operand(0));
        uint32_t index = positionOfParameter[value.index()];
        std::optional<Edge> edge = index == noPosition ? std::nullopt : cfg.singleIncomingEdge(parameters[index].block);
        ValueID passed = edge ? valuePassedOn(function, *edge, parameters[index].index) : ValueID();
        return passed ? candidates.resolve(passed) : ValueID();
    }

    ValueID findNarrowedValueOfEveryEdge(const ParameterPosition& position) const
    {
        Vector<ValueID, 8> common;
        bool hasCommon = false;
        for (Edge edge : cfg.incomingEdges(position.block)) {
            Vector<ValueID, 8> sources;
            for (ValueID value = candidates.resolve(valuePassedOn(function, edge, position.index)); value; value = sourceOfNarrowedValue(value))
                sources.append(value);
            if (sources.contains(position.parameter))
                continue;
            if (!std::exchange(hasCommon, true))
                common = WTF::move(sources);
            else
                common.removeAllMatching([&](ValueID value) { return !sources.contains(value); });
        }
        return common.isEmpty() ? ValueID() : common.first();
    }

    bool tryAssignLeaders(Vector<ParameterPosition>& undecided)
    {
        for (const ParameterPosition& position : undecided)
            leaders[position.parameter.index()] = ValueID();
        for (const ParameterPosition& position : undecided) {
            ValueID leader = computeLeader(position);
            leaders[position.parameter.index()] = leader ? leader : position.parameter;
        }

        Vector<ValueID, 8> refutedParameters;
        for (const ParameterPosition& position : undecided) {
            ValueID leader = leaders[position.parameter.index()];
            if (leader != position.parameter && computeLeader(position) != leader)
                refutedParameters.append(position.parameter);
        }
        for (ValueID parameter : refutedParameters)
            leaders[parameter.index()] = parameter;

        undecided.removeAllMatching([&](const ParameterPosition& position) { return leaders[position.parameter.index()] == position.parameter; });
        return refutedParameters.isEmpty();
    }

    bool findCandidates()
    {
        Vector<ParameterPosition> undecided = parameters;
        for (;;) {
            while (!tryAssignLeaders(undecided)) { }
            if (undecided.isEmpty())
                return !candidates.isEmpty();

            for (const ParameterPosition& position : undecided)
                candidates.replace(position.parameter, leaders[position.parameter.index()]);
            undecided = parameters;
            undecided.removeAllMatching([&](const ParameterPosition& position) { return candidates.resolve(position.parameter) != position.parameter; });
        }
    }

    size_t replaceParametersOf(BlockID id, IRBuilder& builder, ValueSubstitution& replacements)
    {
        struct PendingUpcast {
            ValueID parameter;
            ValueID argument;
            TypeID type;
            TypeID expected;
        };
        const Block& block = function.at(id);
        BitVector removedIndexes;
        Vector<PendingUpcast, 4> upcasts;
        Vector<std::pair<ValueID, KnownValue>, 4> constants;
        for (uint32_t index = 0; index < block.parameters.size(); ++index) {
            ValueID parameter = block.parameters[index];
            ValueID argument = candidates.resolve(parameter);
            if (argument == parameter) {
                if (KnownValue constant = onlyValueOf(block, parameter); constant.isKnown() && positionOfParameter[parameter.index()] != noPosition) {
                    removedIndexes.set(index);
                    constants.append({ parameter, constant });
                }
                continue;
            }
            bool canReplace = canReplaceParameter(function, argument, parameter);
            // A parameter dominates the handler of its block. An Upcast among the values of the block does not. And only a value of the program has an Upcast.
            if (!canReplace && (block.handler || !context.types.at(function.at(parameter).type).isJavaScriptValue()))
                continue;
            removedIndexes.set(index);
            if (canReplace)
                replacements.replace(parameter, argument);
            else
                upcasts.append({ parameter, argument, function.at(parameter).type, function.at(parameter).expected().predicted() });
        }
        builder.removeParameters(id, removedIndexes);
        if (constants.isEmpty() && upcasts.isEmpty())
            return removedIndexes.bitCount();

        uint32_t index = indexAfterCaughtValues(function, block);
        BlockValues& values = function.edit(id, BlockEdit::Values).values;
        for (auto [parameter, constant] : constants) {
            RELEASE_ASSERT(constant.kind() != KnownValue::Kind::Double && constant.kind() != KnownValue::Kind::String);
            becomeConstant(context.program, function, context.types, parameter, constant);
            values.insert(index++, parameter);
        }
        for (const PendingUpcast& upcast : upcasts) {
            Value& value = function.edit(upcast.parameter, ValueEdit::Type);
            value.type = upcast.type;
            value.setExpected(ExpectedType(upcast.expected));
            becomeUpcast(function, upcast.parameter, upcast.argument);
            values.insert(index++, upcast.parameter);
            // The subject is named at the head of the block, above the Upcast.
            if (block.subject == upcast.parameter)
                function.edit(id, BlockEdit::Rest).subject = upcast.argument;
        }
        return removedIndexes.bitCount();
    }
};

} // anonymous namespace

PassOutcome removeTrivialParameters(FunctionPassContext& context)
{
    const CFG& cfg = context.analyses.cfg();
    Function& function = context.function;
    Vector<ParameterPosition> parameters;
    for (BlockID block : cfg.reversePostOrder()) {
        if (block == cfg.root())
            continue;
        const CompactList<ValueID>& blockParameters = function.at(block).parameters;
        for (uint32_t index = 0; index < blockParameters.size(); ++index)
            parameters.append({ blockParameters[index], block, index });
    }
    if (parameters.isEmpty())
        return PassOutcome::Unchanged;

    TrivialParameterRemover remover { context, WTF::move(parameters) };
    for (uint32_t index = 0; index < remover.parameters.size(); ++index)
        remover.positionOfParameter[remover.parameters[index].parameter.index()] = index;
    bool hasWork = remover.findCandidates() || std::ranges::any_of(remover.parameters, [&](const ParameterPosition& position) {
        return remover.onlyValueOf(function.at(position.block), position.parameter).isKnown();
    });
    if (!hasWork)
        return PassOutcome::Unchanged;

    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    ValueSubstitution replacements(function);
    size_t numberOfRemovedParameters = 0;
    for (BlockID block : remover.cfg.reversePostOrder()) {
        if (block != remover.cfg.root())
            numberOfRemovedParameters += remover.replaceParametersOf(block, builder, replacements);
    }
    replacements.apply(function, &context.analyses);
    if (!numberOfRemovedParameters)
        return PassOutcome::Unchanged;
    context.analyses.didChangeValues();
    context.keptAnalyses = allAnalyses;
    return PassOutcome::Changed;
}

// SimplifyControlFlow.

namespace {

enum class HandlerChoice : uint8_t { CannotMerge, KeepPredecessorHandler, TakeSuccessorHandler };

struct ControlFlowSimplifier {
    static constexpr OptionSet<BlockEdit> terminatorAndEdges { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments };

    FunctionPassContext& context;
    const Program& program { context.program };
    Function& function { context.function };
    FunctionAnalyses& analyses { context.analyses };
    IRBuilder builder { analyses.context(), function, context.types, &analyses };
    Vector<std::pair<BlockID, BlockID>, 2> handlerBodies { };
    std::optional<ValueSubstitution> replacements { };
    Vector<ValueID, 8> replacedParameters { };
    BitVector throwingUncoveredBlocks { };
    const UseLists* useListsBeforeMerging { nullptr };
    const Dominators* dominatorsBeforeMerging { nullptr };
    // For hasUsesReachableFromHandler(), from one question to the next.
    Vector<uint32_t> stampOfBlock { };
    uint32_t lastStamp { 0 };
    std::optional<BitVector> blocksLeadingToEdgesBack { };
    size_t numberOfChanges { 0 };
    bool onlyJoinedBlocks { !analyses.cfg().hasHandlers() };
    bool replacedParameterByKnownValue { false };
    bool mayFindMoreAtOnce { false };
    bool movedArgumentsToOtherEdges { false };

    bool leadsTo(BlockID from, BlockID place, unsigned mostSteps) const
    {
        return from == place || (mostSteps && std::ranges::any_of(function.at(from).terminator.targets, [&](const Target& target) { return leadsTo(target.block, place, mostSteps - 1); }));
    }

    bool isLedToByTestBehind(BlockID block, BlockID place) const
    {
        AOT_THRESHOLD(unsigned, mostBlocks, 32);
        AOT_THRESHOLD(unsigned, mostStepsBehindFailure, 3);
        for (unsigned blocks = 0; blocks < mostBlocks; ++blocks) {
            const Terminator& terminator = function.at(block).terminator;
            if (terminator.kind == TerminatorKind::Jump) {
                block = terminator.next().block;
                continue;
            }
            if (terminator.kind == TerminatorKind::SwitchOnShape)
                return leadsTo(terminator.otherwise().block, place, mostStepsBehindFailure);
            if (!terminator.isConditionalBranch())
                return false;
            if (leadsTo(terminator.notTaken().block, place, mostStepsBehindFailure))
                return true;
            if (provesOf(function.at(terminator.conditionOperand()).opcode) == Proof::None)
                return false;
            block = terminator.taken().block;
        }
        return false;
    }

    // Without TestingSwitch::FailPredictions nothing leaves where no test is left.
    void dropFailuresOfPredictionsThatNobodyTests()
    {
        if (!builder.failsPredictions())
            return;
        for (BlockID id : Vector<BlockID>(analyses.cfg().reversePostOrder())) {
            const Terminator& terminator = function.at(id).terminator;
            if (terminator.kind != TerminatorKind::Branch || function.at(terminator.conditionOperand()).opcode != Opcode::ShouldFailPrediction || isLedToByTestBehind(terminator.notTaken().block, terminator.taken().block))
                continue;
            builder.convertBranchToJump(id, 1);
            ++numberOfChanges;
        }
    }

    bool canDecide(Edge edge)
    {
        return canFollowWithoutTest(function, edge) && (!hasEdgeResults(function, edge)
            || argumentsFitParametersAfterCasts(program, function, builder.types(), Target { targetOf(function, edge).block, argumentsWithoutTest(function, edge), { } }));
    }

    void decideBranches(const Vector<BlockID>& reachableBlocks)
    {
        std::optional<TypeTestBranches> typeTests;
        std::optional<SlotValueTests> slotValueTests;
        std::optional<TestsOfSameValues> testsOfSameValues;
        if (context.mayDecideTestsAcrossBlocks) {
            typeTests.emplace(analyses);
            slotValueTests.emplace(analyses, builder.types(), typeTests->blocksThatBranchOnTypeTest());
            testsOfSameValues.emplace(analyses, builder.types());
        }
        auto decide = [&](std::span<const BlockID> blocks, NOESCAPE const Invocable<KnownBoolean(BlockID)> auto& evaluate) {
            Vector<Edge> decided;
            for (BlockID block : blocks) {
                if (function.at(block).terminator.kind != TerminatorKind::Branch)
                    continue;
                Edge edge { block, 0 };
                switch (evaluate(block)) {
                case KnownBoolean::Unknown:
                    continue;
                case KnownBoolean::True:
                    break;
                case KnownBoolean::False:
                    edge.targetIndex = 1;
                    break;
                }
                if (canDecide(edge))
                    decided.append(edge);
            }
            if (decided.isEmpty())
                return false;

            for (Edge edge : decided)
                raiseFrequenciesBehind(analyses, function, edge);
            for (Edge edge : decided)
                decideTerminator(builder, edge.from, edge.targetIndex);
            bool validates = context.build.validates();
            for (Edge edge : decided)
                castArgumentsToParameters(builder, edge.from, validates);
            builder.applyInsertions();
            numberOfChanges += decided.size();
            return true;
        };
        bool didDecide = decide(reachableBlocks.span(), [&](BlockID block) {
            return typeTests ? evaluateCondition(analyses, *typeTests, *slotValueTests, *testsOfSameValues, block) : evaluateConditionByDefinitions(analyses, block);
        });
        mayFindMoreAtOnce |= typeTests && typeTests->gaveUpOnLongWays();
        // With an edge gone, a block may have come to stand behind one edge of a test.
        while (didDecide && testsOfSameValues && !testsOfSameValues->isEmpty()) {
            analyses.didChangeControlFlow();
            if (!testsOfSameValues->didChangeControlFlow())
                break;
            didDecide = decide(analyses.cfg().reversePostOrder(), [&](BlockID block) {
                return testsOfSameValues->outcomeBehindDominatingTests(block);
            });
        }
        if (!testsOfSameValues)
            return;
        auto provenExpectations = testsOfSameValues->takeProvenExpectations();
        for (const auto& proven : provenExpectations)
            proveExpectation(builder, proven);
        builder.applyInsertions();
        numberOfChanges += provenExpectations.size();
    }

    bool simplifyTerminator(BlockID block)
    {
        const Terminator& terminator = function.at(block).terminator;
        switch (terminator.kind) {
        case TerminatorKind::Branch:
            return simplifyBranch(block, terminator);
        case TerminatorKind::Switch:
            return simplifyCases<int64_t>(block, terminator, terminator.cases());
        case TerminatorKind::SwitchString:
            return simplifyCases<StringID>(block, terminator, terminator.strings());
        case TerminatorKind::Jump:
        case TerminatorKind::Return:
        case TerminatorKind::ReturnCall:
        case TerminatorKind::Throw:
        case TerminatorKind::Rethrow:
        case TerminatorKind::Unreachable:
        case TerminatorKind::SwitchOnShape:
        case TerminatorKind::TryCallInline:
        case TerminatorKind::TestAndBranch:
            return false;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    bool simplifyBranch(BlockID block, const Terminator& terminator)
    {
        bool changed = false;
        for (;;) {
            const Value& definition = function.at(definitionThroughUpcasts(function, terminator.conditionOperand()));
            if (definition.opcode != Opcode::LogicalNot)
                break;
            Terminator& negated = function.edit(block, terminatorAndEdges).terminator;
            negated.setConditionOperand(definition.valueOperand());
            std::swap(negated.taken(), negated.notTaken());
            changed = true;
        }
        if (!terminator.taken().proofs.isEmpty() || terminator.taken() != terminator.notTaken())
            return changed;
        decideTerminator(builder, block, 0);
        return true;
    }

    static bool mayStrictlyEqual(const Type& subject, int64_t integer)
    {
        bool mayBeNegativeZero = subject.primitives & Primitive::WholeNumber;
        return (!integer && mayBeNegativeZero) || ((subject.primitives & Primitive::Integer) && subject.integers.contains(integer));
    }

    static bool mayStrictlyEqual(const Type& subject, StringID string)
    {
        return (subject.primitives & Primitive::String) || subject.stringLiterals.contains(string);
    }

    template<typename Case>
    static std::optional<uint64_t> numberOfValuesOf(const Type& subject)
    {
        if (subject.isNever())
            return std::nullopt;
        if constexpr (std::is_same_v<Case, int64_t>) {
            if (!subject.isInteger())
                return std::nullopt;
            return static_cast<uint64_t>(subject.integers.maximum - subject.integers.minimum) + 1;
        } else {
            if (!subject.hasOnlyPrimitives(Primitive::LiteralString))
                return std::nullopt;
            return subject.stringLiterals.size();
        }
    }

    template<typename Case>
    bool simplifyCases(BlockID block, const Terminator& terminator, const auto& cases)
    {
        ValueID subject = terminator.subjectOperand();
        std::optional<uint32_t> decidedTarget = decidedTargetOf(terminator, knownValueOf(program, function, subject));
        if (decidedTarget && canDecide(Edge { block, *decidedTarget })) {
            decideTerminator(builder, block, *decidedTarget);
            return true;
        }

        const Type& subjectType = program.typeOf(function, subject);
        Vector<uint32_t, 8> remainingIndexes;
        uint64_t numberOfListedSubjects = 0;
        bool sharesWayOfRest = false;
        for (uint32_t index = 0; index < cases.size(); ++index) {
            bool mayMatch = mayStrictlyEqual(subjectType, cases[index]);
            numberOfListedSubjects += mayMatch;
            if constexpr (std::is_same_v<Case, int64_t>)
                mayMatch = mayMatch && !isRuledOutByDefinition(program, function, subject, KnownValue::integer(cases[index]));
            if (mayMatch && terminator.targets[index] != terminator.otherwise())
                remainingIndexes.append(index);
            else
                sharesWayOfRest |= mayMatch;
        }
        if (remainingIndexes.isEmpty()) {
            decideTerminator(builder, block, static_cast<uint32_t>(cases.size()));
            return true;
        }
        // A subject whose type is one of the listed integers or strings leaves nothing for the default case. The target that most cases share is no longer tested for: whatever the other cases do not match goes there.
        // Where several of many integers go one way, the bits of a window or the rows of a table tell them apart, and those want each of them listed.
        std::optional<uint32_t> caseThatTakesRest;
        if (!sharesWayOfRest && numberOfListedSubjects == numberOfValuesOf<Case>(subjectType)) {
            // Of the targets that most cases share, the one that is listed last.
            struct SharedTarget {
                uint32_t lastIndex;
                size_t numberOfCases;
            };
            size_t greatestNumberOfCases = 0;
            Vector<uint32_t, 8> indexesByBlock = remainingIndexes;
            AOT::sort(indexesByBlock, { }, [&](uint32_t index) { return std::pair(terminator.targets[index].block, index); });
            Vector<SharedTarget, 2> targetsInBlock;
            for (size_t position = 0; position < indexesByBlock.size(); ++position) {
                uint32_t index = indexesByBlock[position];
                if (position && terminator.targets[indexesByBlock[position - 1]].block != terminator.targets[index].block)
                    targetsInBlock.shrink(0);
                size_t found = targetsInBlock.findIf([&](const SharedTarget& shared) { return terminator.targets[shared.lastIndex] == terminator.targets[index]; });
                if (found == notFound) {
                    found = targetsInBlock.size();
                    targetsInBlock.append(SharedTarget { index, 0 });
                }
                SharedTarget& shared = targetsInBlock[found];
                shared.lastIndex = index;
                ++shared.numberOfCases;
                if (shared.numberOfCases > greatestNumberOfCases || (shared.numberOfCases == greatestNumberOfCases && index > *caseThatTakesRest)) {
                    greatestNumberOfCases = shared.numberOfCases;
                    caseThatTakesRest = index;
                }
            }
            bool isDistinguishedByWindow = std::is_same_v<Case, int64_t> && greatestNumberOfCases > 1 && numberOfListedSubjects >= SwitchWindow::minCases;
            if (isDistinguishedByWindow || !canDecide(Edge { block, *caseThatTakesRest }))
                caseThatTakesRest = std::nullopt;
        }
        bool restIsNarrowed = caseThatTakesRest && hasEdgeResults(function, Edge { block, *caseThatTakesRest });
        if (caseThatTakesRest) {
            remainingIndexes.removeAllMatching([&](uint32_t index) { return terminator.targets[index] == terminator.targets[*caseThatTakesRest]; });
            if (remainingIndexes.isEmpty()) {
                decideTerminator(builder, block, *caseThatTakesRest);
                castArgumentsToParameters(builder, block, context.build.validates());
                builder.applyInsertions();
                return true;
            }
        } else if (remainingIndexes.size() == cases.size())
            return false;

        Arguments argumentsOfRest;
        if (restIsNarrowed)
            argumentsOfRest = argumentsWithoutTest(function, Edge { block, *caseThatTakesRest });
        Terminator& edited = function.edit(block, terminatorAndEdges).terminator;
        if (restIsNarrowed)
            edited.otherwise() = Target { edited.targets[*caseThatTakesRest].block, WTF::move(argumentsOfRest), { } };
        else if (caseThatTakesRest)
            edited.otherwise() = WTF::move(edited.targets[*caseThatTakesRest]);
        Vector<Case, 8> remainingCases;
        Vector<Target, 2> remainingTargets;
        for (uint32_t index : remainingIndexes) {
            remainingCases.append(cases[index]);
            remainingTargets.append(WTF::move(edited.targets[index]));
        }
        remainingTargets.append(WTF::move(edited.otherwise()));
        if constexpr (std::is_same_v<Case, int64_t>)
            edited.setCases(remainingCases.span());
        else
            edited.setStrings(remainingCases.span());
        edited.targets = WTF::move(remainingTargets);
        if (restIsNarrowed)
            narrowArgumentsOfRest(block);
        return true;
    }

    // The default path narrows nothing by itself: a block on it passes the subject on as the case narrowed it.
    void narrowArgumentsOfRest(BlockID block)
    {
        BlockID stub = builder.addBlock(function.at(function.at(block).terminator.otherwise().block).frequency);
        Terminator& terminator = function.edit(block, terminatorAndEdges).terminator;
        Target rest = std::exchange(terminator.otherwise(), Target { stub, { }, { } });
        Origin previousOrigin = builder.origin();
        BlockID previousBlock = builder.currentBlock();

        builder.setOrigin(terminator.origin());
        builder.moveToEndOf(stub);
        builder.emitJump(rest.block, WTF::move(rest.arguments));
        castArgumentsToParameters(builder, stub, context.build.validates());
        builder.applyInsertions();

        builder.setOrigin(previousOrigin);
        if (previousBlock)
            builder.moveToEndOf(previousBlock);
    }

    void bypassHandlers()
    {
        if (!analyses.cfg().hasHandlers())
            return;

        Vector<std::pair<BlockID, BitVector>> bypassedThrows;
        for (BlockID block : analyses.cfg().reversePostOrder()) {
            if (throwsIntoIgnoringHandler(block))
                bypassedThrows.append({ block, findDeadValuesBeforeThrow(block) });
        }
        for (const auto& [block, deadValues] : bypassedThrows) {
            if (!bypassHandler(block) || deadValues.isEmpty())
                continue;
            for (size_t index : deadValues)
                explainOperationThatGoes(function, function.at(block).values[index], InPlaceOfOperation::NothingBecauseNobodySeesHowItEnds);
            builder.eraseValues(block, deadValues);
        }
        for (size_t index = 0; index < handlerBodies.size(); ++index)
            bypassHandler(handlerBodies[index].second);
    }

    bool throwsIntoIgnoringHandler(BlockID id) const
    {
        const Block& block = function.at(id);
        TerminatorKind kind = block.terminator.kind;
        if ((kind != TerminatorKind::Throw && kind != TerminatorKind::Rethrow) || !block.handler)
            return false;
        BlockID handler = block.handler->block;
        const Block& handlerBlock = function.at(handler);
        return handler != id && handlerBlock.role == BlockRole::CatchHandler && !indexAfterCaughtValues(function, handlerBlock);
    }

    // Whether such an operation completes or throws, the code of the handler runs next with the same arguments, and it sees neither a result nor an exception.
    BitVector findDeadValuesBeforeThrow(BlockID id)
    {
        const Block& block = function.at(id);
        const UseLists& useLists = analyses.useLists();
        BitVector indexes(block.values.size());
        for (size_t index = block.values.size(); index--;) {
            ValueID value = block.values[index];
            Effects effects = analyses.effectsOf(value).onAnyEdge();
            effects.mayThrow = false;
            // A user in the block comes later, so it is among the operations found.
            bool isUsedElsewhere = std::ranges::any_of(useLists.usesOf(value), [&](const Use& use) {
                switch (use.kind) {
                case Use::Kind::Operand:
                case Use::Kind::TerminatorOperand:
                    return use.block != id;
                case Use::Kind::EdgeArgument:
                case Use::Kind::HandlerArgument:
                case Use::Kind::BlockSubject:
                    return true;
                }
                RELEASE_ASSERT_NOT_REACHED();
            });
            if (isUsedElsewhere || !effects.isRemovableWhenUnused())
                break;
            indexes.quickSet(index);
        }
        return indexes;
    }

    bool bypassHandler(BlockID id)
    {
        if (!throwsIntoIgnoringHandler(id))
            return false;

        BlockID body = bodyOf(function.at(id).handler->block);
        Block& throwingBlock = function.edit(id, terminatorAndEdges);
        Arguments arguments = throwingBlock.handler->arguments;
        builder.setOrigin(throwingBlock.terminator.origin());
        throwingBlock.terminator = Terminator { };
        builder.moveToEndOf(id);
        builder.emitJump(body, WTF::move(arguments));
        // The builder reports the jump, not the terminator in whose place it stands.
        analyses.didChangeControlFlow();
        ++numberOfChanges;
        return true;
    }

    // The first to ask moves the body out of the handler.
    BlockID bodyOf(BlockID handler)
    {
        for (auto [knownHandler, body] : handlerBodies) {
            if (knownHandler == handler)
                return body;
        }
        BlockID body = builder.addBlock(function.at(handler).frequency);
        handlerBodies.append({ handler, body });
        Arguments handlerParameters = function.at(handler).parameters;
        ValueSubstitution substitution(function);
        for (ValueID parameter : handlerParameters) {
            builder.setOrigin(function.at(parameter).origin());
            ValueID bodyParameter = builder.addParameter(body, function.at(parameter).type);
            function.edit(bodyParameter, ValueEdit::Type).setExpected(function.at(parameter).expected());
            substitution.replace(parameter, bodyParameter);
        }

        Block& source = function.edit(handler, terminatorAndEdges | BlockEdit::Values);
        Block& destination = function.edit(body, terminatorAndEdges | BlockEdit::Values);
        destination.values = std::exchange(source.values, { });
        destination.terminator = std::exchange(source.terminator, Terminator { });
        destination.handler = std::exchange(source.handler, std::nullopt);
        substitution.apply(function, &analyses);

        builder.setOrigin(destination.terminator.origin());
        builder.moveToEndOf(handler);
        builder.emitJump(body, WTF::move(handlerParameters));
        // The builder reports the jump, not what has moved into the body.
        analyses.didChangeControlFlow();
        return body;
    }

    bool canForwardThrough(BlockID id) const
    {
        const Block& block = function.at(id);
        if (!block.parameters.isEmpty() || !block.values.isEmpty() || block.isHandlerBlock() || block.terminator.kind != TerminatorKind::Jump)
            return false;
        // The block writes its arguments once. Led past it, every edge would write them.
        size_t numberOfArguments = block.terminator.next().arguments.size();
        size_t numberOfIncomingEdges = analyses.cfg().incomingEdges(id).size();
        if (numberOfIncomingEdges * numberOfArguments > numberOfIncomingEdges + numberOfArguments)
            return false;
        BlockID target = block.terminator.next().block;
        Expected<BlockFrequency> targetFrequency = function.at(target).frequency;
        return target != id && rarerOf(block.frequency, targetFrequency).isSamePredictionAs(targetFrequency);
    }

    static ValueID testedValueOf(const Terminator& terminator)
    {
        switch (terminator.kind) {
        case TerminatorKind::Branch:
            return terminator.conditionOperand();
        case TerminatorKind::Switch:
        case TerminatorKind::SwitchString:
            return terminator.subjectOperand();
        case TerminatorKind::Jump:
        case TerminatorKind::Return:
        case TerminatorKind::ReturnCall:
        case TerminatorKind::Throw:
        case TerminatorKind::Rethrow:
        case TerminatorKind::Unreachable:
        case TerminatorKind::SwitchOnShape:
        case TerminatorKind::TryCallInline:
        case TerminatorKind::TestAndBranch:
            return { };
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    // The parameter of the arm that the value is, as it is or behind casts made in the arm.
    ValueID narrowedValueBehindCasts(const Block& arm, ValueID id) const
    {
        while (isCastWithoutTest(function.at(id).opcode) && arm.values.contains(id))
            id = function.at(id).operand(0);
        return arm.parameters.contains(id) ? id : ValueID();
    }

    using ArmOperations = Vector<ValueID, 8>;

    // Casts of what the edge narrowed, and the facts that they name, say what the test found. They compute nothing.
    ArmOperations operationsOf(const Block& arm) const
    {
        ArmOperations operations;
        for (ValueID id : arm.values) {
            Opcode opcode = function.at(id).opcode;
            if (opcode != Opcode::Specified && !(isCastWithoutTest(opcode) && narrowedValueBehindCasts(arm, id)))
                operations.append(id);
        }
        return operations;
    }

    // `const pair = { a: given }` tests `given` for the type of the field. Once the object is never allocated, both arms only store the value or pass it on:
    // neither looks at it, so the test makes neither of them faster. The arms are copies of the same operations of the source, so they also throw at the same positions.
    bool haveSameCode(BlockID from) const
    {
        AOT_THRESHOLD(size_t, maxValuesInSameArms, 12);
        const CFG& cfg = analyses.cfg();
        const Terminator& branch = function.at(from).terminator;
        BlockID taken = branch.taken().block;
        BlockID notTaken = branch.notTaken().block;
        if (taken == notTaken || cfg.incomingEdges(taken).size() != 1 || cfg.incomingEdges(notTaken).size() != 1)
            return false;
        const Block& first = function.at(taken);
        const Block& second = function.at(notTaken);
        if (first.terminator.kind != second.terminator.kind || first.parameters.size() != second.parameters.size() || std::max(first.values.size(), second.values.size()) > maxValuesInSameArms)
            return false;
        if (first.isHandlerBlock() || second.isHandlerBlock() || !haveSameHandler(first, second))
            return false;
        // A test of an element also asks whether the element is there. What it finds makes the read itself cheaper, whatever the arms do with the value.
        if (const Value& test = function.at(branch.conditionOperand()); test.opcode == Opcode::TypeTest && function.at(sameObjectThroughCasts(function, test.valueOperand())).opcode == Opcode::GetArrayElement)
            return false;
        Edge takenEdge { from, 0 };
        Edge notTakenEdge { from, 1 };
        if (!canFollowWithoutTest(function, takenEdge) || !canFollowWithoutTest(function, notTakenEdge) || argumentsWithoutTest(function, takenEdge) != argumentsWithoutTest(function, notTakenEdge))
            return false;
        ArmOperations firstOperations = operationsOf(first);
        ArmOperations secondOperations = operationsOf(second);

        // With the test goes what it found, and with the casts what was proven of the value behind it. Whatever takes the value has to take all that the test started from.
        Arguments given = argumentsWithoutTest(function, takenEdge);
        TypingContext typing { program, function, context.types };
        auto isTakenAs = [&](const Block& arm, ValueID narrowed, TypeID required) {
            return !required || program.typeOf(function, given[arm.parameters.find(narrowed)]).isSubsetOf(context.types.at(required), program);
        };
        auto takesWhatWasGiven = [&]<typename User>(const Block& arm, const User& user, uint32_t index, ValueID narrowed) {
            if constexpr (std::is_same_v<User, Value>) {
                if (user.operandKind(index) != OperandKind::Any && user.operandKind(index) != OperandKind::Declared)
                    return false;
            }
            return isTakenAs(arm, narrowed, requiredTypeOfOperand(typing, user, index));
        };

        auto isFactOf = [&](const Block& arm, ValueID id) {
            return function.at(id).opcode == Opcode::Specified && arm.values.contains(id);
        };
        auto isSameValue = [&](ValueID inFirst, ValueID inSecond) {
            if (!inFirst || !inSecond)
                return inFirst == inSecond;
            if (ValueID narrowed = narrowedValueBehindCasts(first, inFirst))
                return narrowedValueBehindCasts(second, inSecond) == second.parameters[first.parameters.find(narrowed)];
            if (size_t index = firstOperations.find(inFirst); index != notFound)
                return secondOperations[index] == inSecond;
            return inFirst == inSecond || (isFactOf(first, inFirst) && isFactOf(second, inSecond));
        };
        auto haveSameOperands = [&](const auto& inFirst, const auto& inSecond) {
            if (inFirst.origin() != inSecond.origin() || !inFirst.hasSamePayloadAs(inSecond) || inFirst.numberOfNamedOperands() != inSecond.numberOfNamedOperands() || !std::ranges::equal(inFirst.operands(), inSecond.operands(), isSameValue))
                return false;
            std::span<const ValueID> operands = inFirst.operands();
            for (uint32_t index = 0; index < operands.size(); ++index) {
                ValueID narrowed = operands[index] ? narrowedValueBehindCasts(first, operands[index]) : ValueID();
                if (narrowed && (!inFirst.operandUses(index).containsOnly({ OperandUse::Retains, OperandUse::Returns }) || !takesWhatWasGiven(first, inFirst, index, narrowed)))
                    return false;
            }
            return true;
        };
        bool haveSameOperations = std::ranges::equal(firstOperations, secondOperations, [&](ValueID inFirst, ValueID inSecond) {
            const Value& a = function.at(inFirst);
            const Value& b = function.at(inSecond);
            return a.opcode == b.opcode && a.type == b.type && a.expected().predicted() == b.expected().predicted() && haveSameOperands(a, b);
        });
        if (!haveSameOperations || !haveSameOperands(first.terminator, second.terminator))
            return false;
        return std::ranges::equal(first.terminator.targets, second.terminator.targets, [&](const Target& a, const Target& b) {
            if (a.block != b.block || a.proofs != b.proofs || !std::ranges::equal(a.arguments, b.arguments, isSameValue))
                return false;
            for (size_t index = 0; index < a.arguments.size(); ++index) {
                ValueID narrowed = narrowedValueBehindCasts(first, a.arguments[index]);
                if (narrowed && !isTakenAs(first, narrowed, function.at(function.at(a.block).parameters[index]).type))
                    return false;
            }
            return true;
        });
    }

    bool hasArmsWithSameCode() const
    {
        return std::ranges::any_of(analyses.cfg().reversePostOrder(), [&](BlockID block) {
            return function.at(block).terminator.kind == TerminatorKind::Branch && haveSameCode(block);
        });
    }

    void mergeArmsWithSameCode()
    {
        Vector<Edge, 4> kept;
        for (BlockID block : analyses.cfg().reversePostOrder()) {
            const Terminator& terminator = function.at(block).terminator;
            if (terminator.kind != TerminatorKind::Branch || !haveSameCode(block))
                continue;
            bool takenIsRarer = isRarerThan(function.at(terminator.taken().block).frequency, function.at(terminator.notTaken().block).frequency).predicted();
            kept.append(Edge { block, takenIsRarer ? 1u : 0u });
        }
        if (kept.isEmpty())
            return;
        for (Edge edge : kept)
            raiseFrequenciesBehind(analyses, function, edge);
        for (Edge edge : kept) {
            decideTerminator(builder, edge.from, edge.targetIndex);
            // The arm now takes what either edge narrowed, so what its casts said of the value no longer holds.
            const Target& next = function.at(edge.from).terminator.next();
            BlockID arm = next.block;
            for (size_t index = 0; index < next.arguments.size(); ++index) {
                const Value& argument = function.at(next.arguments[index]);
                Value& parameter = function.edit(function.at(arm).parameters[index], ValueEdit::Type);
                parameter.type = argument.type;
                parameter.setExpected(argument.expected());
            }
            ValueSubstitution casts(function);
            BitVector positionsOfCasts;
            std::span<const ValueID> values = function.at(arm).values.span();
            for (size_t position = 0; position < values.size(); ++position) {
                ValueID narrowed = isCastWithoutTest(function.at(values[position]).opcode) ? narrowedValueBehindCasts(function.at(arm), values[position]) : ValueID();
                if (!narrowed)
                    continue;
                casts.replace(values[position], narrowed);
                positionsOfCasts.set(position);
            }
            if (casts.isEmpty())
                continue;
            casts.apply(function, &analyses);
            builder.eraseValues(arm, positionsOfCasts);
        }
        numberOfChanges += kept.size();
        onlyJoinedBlocks = false;
        analyses.didChangeControlFlow();
    }

    void collapseTestsThatDecideNothing()
    {
        Vector<BlockID, 8> sources = forwardEmptyBlocks();
        while (!sources.isEmpty()) {
            Vector<ValueID, 8> testedValues;
            size_t numberOfChangesBefore = numberOfChanges;
            for (BlockID block : sources) {
                ValueID tested = testedValueOf(function.at(block).terminator);
                if (!tested || !simplifyTerminator(block))
                    continue;
                ++numberOfChanges;
                if (function.at(block).terminator.kind == TerminatorKind::Jump)
                    testedValues.append(tested);
            }
            if (numberOfChanges != numberOfChangesBefore) {
                onlyJoinedBlocks = false;
                analyses.didChangeControlFlow();
            }
            if (testedValues.isEmpty())
                return;
            sources = eraseUnusedValues(WTF::move(testedValues));
            sources.appendVector(forwardEmptyBlocks());
        }
    }

    Vector<BlockID, 8> eraseUnusedValues(Vector<ValueID, 8>&& worklist)
    {
        const CFG& cfg = analyses.cfg();
        const UseLists& useLists = analyses.useLists();
        HashMap<ValueID, unsigned> numberOfErasedUses;
        Vector<ValueLocation, 8> erased;
        auto eraseUseOf = [&](ValueID used) {
            ++numberOfErasedUses.add(used, 0).iterator->value;
            worklist.append(used);
        };
        while (!worklist.isEmpty()) {
            ValueID id = worklist.takeLast();
            ValueLocation location = useLists.locationOf(id);
            if (useLists.useCount(id) != numberOfErasedUses.get(id) || erased.contains(location))
                continue;
            switch (location.placement) {
            case ValuePlacement::Detached:
                continue;
            case ValuePlacement::Parameter:
                if (location.block == cfg.root())
                    continue;
                for (Edge edge : cfg.incomingEdges(location.block)) {
                    IncomingValue incoming = incomingValueOf(function, edge, location.index);
                    if (incoming.kind == IncomingValue::Kind::Argument)
                        eraseUseOf(incoming.argument);
                }
                break;
            case ValuePlacement::InBlock:
                if (!analyses.isRemovableWhenUnused(id) && !analyses.context().build.isSabotaged(Sabotage::ErasedValuesHaveEffects))
                    continue;
                function.at(id).forEachOperand([&](ValueID operand, uint32_t) { eraseUseOf(operand); });
                break;
            }
            erased.append(location);
        }
        if (erased.isEmpty())
            return { };

        Vector<BlockID, 8> sources;
        for (const ValueLocation& location : erased) {
            if (location.placement != ValuePlacement::Parameter)
                continue;
            for (Edge edge : cfg.incomingEdges(location.block)) {
                if (!edge.isHandlerEdge() && !sources.contains(edge.from))
                    sources.append(edge.from);
            }
        }

        AOT::sort(erased, { }, [](const ValueLocation& location) { return std::tuple(location.block, location.placement, location.index); });
        for (size_t first = 0; first < erased.size();) {
            size_t end = first;
            BitVector indexes;
            while (end < erased.size() && erased[end].block == erased[first].block && erased[end].placement == erased[first].placement)
                indexes.set(erased[end++].index);
            if (erased[first].placement == ValuePlacement::Parameter)
                builder.removeParameters(erased[first].block, indexes);
            else
                builder.eraseValues(erased[first].block, indexes);
            first = end;
        }
        numberOfChanges += erased.size();
        return sources;
    }

    Vector<BlockID, 8> forwardEmptyBlocks()
    {
        Vector<Edge, 8> forwardedEdges;
        for (BlockID from : analyses.cfg().reversePostOrder()) {
            const Terminator& terminator = function.at(from).terminator;
            if (terminator.kind == TerminatorKind::Jump)
                continue;
            for (uint32_t targetIndex = 0; targetIndex < terminator.targets.size(); ++targetIndex) {
                if (canForwardThrough(terminator.targets[targetIndex].block))
                    forwardedEdges.append(Edge { from, targetIndex });
            }
        }
        if (forwardedEdges.isEmpty())
            return { };

        forwardedEdges.removeAllMatching([&](const Edge& edge) { return analyses.isInCycle(targetOf(function, edge).block); });
        Vector<BlockID, 8> lastEmptyBlocks(forwardedEdges.size(), [&](size_t index) {
            BlockID last = targetOf(function, forwardedEdges[index]).block;
            for (;;) {
                BlockID next = function.at(last).terminator.next().block;
                if (!canForwardThrough(next) || analyses.isInCycle(next))
                    return last;
                last = next;
            }
        });

        Vector<BlockID, 8> sources;
        for (size_t index = 0; index < forwardedEdges.size(); ++index) {
            Edge edge = forwardedEdges[index];
            const Target& next = function.at(lastEmptyBlocks[index]).terminator.next();
            movedArgumentsToOtherEdges |= !next.arguments.isEmpty();
            builder.redirectEdge(edge, next.block, Arguments(next.arguments));
            if (sources.isEmpty() || sources.last() != edge.from)
                sources.append(edge.from);
        }
        numberOfChanges += forwardedEdges.size();
        return sources;
    }

    std::optional<BlockID> findMergeableSuccessor(const CFG& cfg, BlockID id) const
    {
        const Terminator& terminator = function.at(id).terminator;
        if (terminator.kind != TerminatorKind::Jump)
            return std::nullopt;
        BlockID next = terminator.next().block;
        if (next == id || next == cfg.root() || cfg.incomingEdges(next).size() != 1 || function.at(next).isHandlerBlock())
            return std::nullopt;
        return next;
    }

    ValueID resolve(ValueID id) const { return replacements ? replacements->resolve(id) : id; }

    bool haveSameHandler(const Block& block, const Block& successor) const
    {
        if (!block.handler || !successor.handler)
            return !block.handler && !successor.handler;
        return block.handler->block == successor.handler->block && std::ranges::equal(block.handler->arguments, successor.handler->arguments, [&](ValueID argument, ValueID successorArgument) { return resolve(argument) == resolve(successorArgument); });
    }

    // From any other block every way leads to blocks that come later in reverse post-order, and to no others.
    const BitVector& findBlocksLeadingToEdgesBack(const CFG& cfg)
    {
        if (!blocksLeadingToEdgesBack) {
            blocksLeadingToEdgesBack.emplace(cfg.numNodes());
            std::span<const BlockID> order = cfg.reversePostOrder();
            for (size_t index = cfg.hasCycle() ? order.size() : 0; index--;) {
                bool leadsBack = std::ranges::any_of(cfg.successors(order[index]), [&](BlockID successor) {
                    return cfg.reversePostOrderIndex(successor) <= index || blocksLeadingToEdgesBack->quickGet(successor.index());
                });
                if (leadsBack)
                    blocksLeadingToEdgesBack->quickSet(order[index].index());
            }
        }
        return *blocksLeadingToEdgesBack;
    }

    // Whether control that enters the handler of `next` can come to a use of what `id` defines without coming through `id` again.
    // A handler may lead back into the function, as that of a call whose result is awaited does. So the walk from it leaves out whatever lies behind the last use, and it ends
    // at a block through which every way to that use leads: a function of n such calls would otherwise be walked to its end n times.
    bool hasUsesReachableFromHandler(const CFG& cfg, BlockID id, BlockID next)
    {
        RELEASE_ASSERT(useListsBeforeMerging && dominatorsBeforeMerging);
        const Block& block = function.at(id);
        const Block& successor = function.at(next);
        if (stampOfBlock.isEmpty())
            stampOfBlock.fill(0, cfg.numNodes());
        const uint32_t holdsUse = ++lastStamp;
        const uint32_t wasVisited = ++lastStamp;
        BlockID lastBlockWithUse;
        auto isArgumentOfHandler = [&](ValueID definition) {
            bool isArgument = false;
            for (const Use& use : useListsBeforeMerging->usesOf(definition)) {
                isArgument |= use.kind == Use::Kind::HandlerArgument && use.block == next;
                if (use.block == id || !cfg.isReachable(use.block))
                    continue;
                stampOfBlock[use.block.index()] = holdsUse;
                if (!lastBlockWithUse || cfg.reversePostOrderIndex(use.block) > cfg.reversePostOrderIndex(lastBlockWithUse))
                    lastBlockWithUse = use.block;
            }
            return isArgument;
        };
        bool isArgument = std::ranges::any_of(block.values, isArgumentOfHandler)
            || std::ranges::any_of(successor.parameters, isArgumentOfHandler)
            || std::ranges::any_of(replacedParameters, [&](ValueID parameter) { return block.values.contains(resolve(parameter)) && isArgumentOfHandler(parameter); });
        if (isArgument || !lastBlockWithUse)
            return isArgument;

        // Every way to a use leads through `id`. So does every way from the handler to one, where the handler can be entered without coming through `id`.
        const Dominators& dominators = *dominatorsBeforeMerging;
        BlockID handler = successor.handler->block;
        if (!dominators.dominates(id, handler))
            return false;
        const BitVector& leadsToEdgeBack = findBlocksLeadingToEdgesBack(cfg);
        const uint32_t indexOfLastUse = cfg.reversePostOrderIndex(lastBlockWithUse);
        Vector<BlockID, 16> worklist;
        auto enter = [&](BlockID entered) {
            uint32_t& stamp = stampOfBlock[entered.index()];
            if (stamp == holdsUse)
                return true;
            if (entered == id || stamp == wasVisited)
                return false;
            stamp = wasVisited;
            if (cfg.reversePostOrderIndex(entered) > indexOfLastUse && !leadsToEdgeBack.quickGet(entered.index()))
                return false;
            if (dominators.strictlyDominates(id, entered) && dominators.dominates(entered, lastBlockWithUse))
                return true;
            worklist.append(entered);
            return false;
        };
        if (enter(handler))
            return true;
        while (!worklist.isEmpty()) {
            if (std::ranges::any_of(cfg.successors(worklist.takeLast()), enter))
                return true;
        }
        return false;
    }

    HandlerChoice chooseHandler(const CFG& cfg, BlockID id, BlockID next)
    {
        const Block& block = function.at(id);
        const Block& successor = function.at(next);
        if (haveSameHandler(block, successor))
            return HandlerChoice::KeepPredecessorHandler;

        if (!successor.handler) {
            bool canBeCovered = !throwingUncoveredBlocks.get(next.index()) && successor.terminator.kind != TerminatorKind::ReturnCall;
            return canBeCovered ? HandlerChoice::KeepPredecessorHandler : HandlerChoice::CannotMerge;
        }

        if (block.handler || throwingUncoveredBlocks.get(id.index()) || hasUsesReachableFromHandler(cfg, id, next))
            return HandlerChoice::CannotMerge;
        return HandlerChoice::TakeSuccessorHandler;
    }

    bool mergeSuccessorInto(const CFG& cfg, BlockID id)
    {
        std::optional<BlockID> next = findMergeableSuccessor(cfg, id);
        if (!next)
            return false;
        HandlerChoice choice = chooseHandler(cfg, id, *next);
        if (choice == HandlerChoice::CannotMerge)
            return false;

        if (!replacements)
            replacements.emplace(function);
        Block& block = function.edit(id);
        Arguments arguments = WTF::move(block.terminator.next().arguments);
        Block successor = std::exchange(function.edit(*next), Block { });
        onlyJoinedBlocks &= successor.parameters.isEmpty();
        for (unsigned index = 0; index < successor.parameters.size(); ++index) {
            ValueID parameter = successor.parameters[index];
            replacedParameterByKnownValue = replacedParameterByKnownValue || function.at(arguments[index]).type != function.at(parameter).type || knownValueOf(program, function, arguments[index]).isKnown();
            if (canReplaceParameter(function, arguments[index], parameter)) {
                replacements->replace(parameter, arguments[index]);
                replacedParameters.append(parameter);
                continue;
            }
            becomeUpcast(function, parameter, arguments[index]);
            block.values.append(parameter);
        }
        block.values.appendVector(successor.values);
        block.terminator = WTF::move(successor.terminator);
        if (choice == HandlerChoice::TakeSuccessorHandler)
            block.handler = WTF::move(successor.handler);
        mayFindMoreAtOnce |= !rarerOf(block.frequency, successor.frequency).isSamePredictionAs(block.frequency) || (block.values.isEmpty() && block.parameters.isEmpty());
        block.frequency = rarerOf(block.frequency, successor.frequency);
        if (throwingUncoveredBlocks.get(next->index()))
            throwingUncoveredBlocks.set(id.index());
        ++numberOfChanges;
        return true;
    }

    void mergeChains(const CFG& cfg)
    {
        auto mayThrowIn = [&](BlockID id) {
            return analyses.effectTraitsOfTerminator(id).normal.mayThrow() || std::ranges::any_of(function.at(id).values, [&](ValueID value) { return analyses.mayThrow(value); });
        };
        for (BlockID id : cfg.hasHandlers() ? cfg.reversePostOrder() : std::span<const BlockID> { }) {
            std::optional<BlockID> next = findMergeableSuccessor(cfg, id);
            if (!next)
                continue;
            for (BlockID member : { id, *next }) {
                if (!function.at(member).handler && !throwingUncoveredBlocks.get(member.index()) && mayThrowIn(member))
                    throwingUncoveredBlocks.set(member.index());
            }
            if (!function.at(id).handler && function.at(*next).handler) {
                useListsBeforeMerging = &analyses.useLists();
                dominatorsBeforeMerging = &analyses.dominators();
            }
        }
        for (BlockID id : cfg.reversePostOrder()) {
            replacedParameters.shrink(0);
            while (mergeSuccessorInto(cfg, id)) { }
        }
        if (replacements)
            replacements->apply(function, &analyses);
    }
};

} // anonymous namespace

PassOutcome simplifyControlFlow(FunctionPassContext& context)
{
    ControlFlowSimplifier simplifier { context };
    FunctionAnalyses& analyses = context.analyses;
    size_t& numberOfChanges = simplifier.numberOfChanges;
    Vector<BlockID> reachableBlocks(analyses.cfg().reversePostOrder());
    simplifier.decideBranches(reachableBlocks);
    for (BlockID block : reachableBlocks)
        numberOfChanges += simplifier.simplifyTerminator(block);
    simplifier.dropFailuresOfPredictionsThatNobodyTests();
    if (numberOfChanges)
        analyses.didChangeControlFlow();

    simplifier.bypassHandlers();
    simplifier.onlyJoinedBlocks &= !numberOfChanges;
    simplifier.mergeArmsWithSameCode();
    simplifier.collapseTestsThatDecideNothing();
    numberOfChanges += simplifier.builder.emptyUnreachableBlocks();
    size_t numberOfChangesBeforeMerging = numberOfChanges;
    simplifier.mergeChains(analyses.cfg());
    if (numberOfChanges != numberOfChangesBeforeMerging)
        analyses.didChangeControlFlow();

    context.lookedOnlyNearby = !context.mayDecideTestsAcrossBlocks;
    if (!numberOfChanges)
        return PassOutcome::Unchanged;
    context.keptAnalyses = allAnalyses;
    if (!simplifier.replacedParameterByKnownValue)
        context.passesThatCanWait.add(PassID::FoldConstants);
    if (!simplifier.onlyJoinedBlocks)
        return PassOutcome::Changed;
    context.passesLeftIdle = { PassID::RemoveTrivialParameters, PassID::EliminateDeadCode };
    if (!simplifier.movedArgumentsToOtherEdges)
        context.passesLeftIdle.add(PassID::PropagateBranchConditions);
    if (!context.foldingAskedForUses)
        context.passesLeftIdle.add(PassID::FoldConstants);
    // Two arms of several blocks each are compared once their blocks are joined.
    context.reachedFixpoint = !simplifier.mayFindMoreAtOnce && !simplifier.hasArmsWithSameCode();
    return PassOutcome::Changed;
}

// ReduceIntegerDivisions.

static bool isExactInteger(const Type& type, int64_t limit)
{
    return type.isInteger() && type.integers.isSubsetOf({ -limit, limit });
}

static bool canDivideAsIntegers(const BuildContext& build, QuotientRounding rounding, const Type& dividend, const Type& divisor)
{
    int64_t limit = rounding == QuotientRounding::Round ? maxExactInteger >> 2 : maxExactInteger;
    if (!isExactInteger(dividend, limit) || !isExactInteger(divisor, limit))
        return false;
    Range dividends = dividend.integers;
    Range divisors = divisor.integers;
    if (dividends.minimum == dividends.maximum && divisors.minimum == divisors.maximum)
        return false;
    if (rounding == QuotientRounding::ToInt32)
        return true;
    if (rounding == QuotientRounding::Floor)
        return divisors.minimum > 0 || build.isSabotaged(Sabotage::FlooredQuotientsHaveAnyDivisor);
    return (divisors.minimum > 0 && dividends.minimum >= 0) || build.isSabotaged(Sabotage::RoundedQuotientsHaveAnySign);
}

static std::optional<Value> findIntegerDivisionFor(Rewrite& rewrite, ValueID operand, QuotientRounding rounding)
{
    const Value& quotient = rewrite.function.at(sameObjectThroughCasts(rewrite.function, operand));
    if (quotient.opcode != Opcode::Div || !canDivideAsIntegers(rewrite.context.build, rounding, rewrite.typeOf(quotient.leftOperand()), rewrite.typeOf(quotient.rightOperand())))
        return std::nullopt;
    Value division = IRBuilder::make(Opcode::DivideIntegers, { quotient.leftOperand(), quotient.rightOperand() });
    division.setRounding(rounding);
    return division;
}

static std::optional<QuotientRounding> roundingOf(std::optional<InlineBuiltin> builtin)
{
    if (!builtin)
        return std::nullopt;
    switch (*builtin) {
    case InlineBuiltin::MathFloor:
        return QuotientRounding::Floor;
    case InlineBuiltin::MathCeil:
        return QuotientRounding::Ceil;
    case InlineBuiltin::MathTrunc:
        return QuotientRounding::Truncate;
    case InlineBuiltin::MathRound:
        return QuotientRounding::Round;
    default:
        return std::nullopt;
    }
}

static void reduceRoundedQuotient(Rewrite& rewrite, const Site& call)
{
    std::optional<QuotientRounding> rounding = roundingOf(rewrite.program.builtins().at(call.value.overload()).code.implementation.inlineBuiltin);
    if (!rounding || call.value.builtinCallForm() != BuiltinCallForm::Whole || call.value.argumentsOperands().size() != 1)
        return;
    if (std::optional<Value> division = findIntegerDivisionFor(rewrite, call.value.argumentsOperands()[0], *rounding)) {
        rewrite.later([&rewrite, id = call.id, division = WTF::move(*division)]() mutable {
            rewrite.become(id, WTF::move(division));
            rewrite.count(StatisticID::IntegerDivisionsReduced);
        });
    }
}

static void reduceQuotientConvertedToInt32(Rewrite& rewrite, const Site& user)
{
    user.value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        std::optional<Value> division = findIntegerDivisionFor(rewrite, operand, QuotientRounding::ToInt32);
        if (!division)
            return;
        rewrite.later([&rewrite, point = user.point, id = user.id, operandIndex, division = WTF::move(*division)]() mutable {
            ValueID quotient = rewrite.insertBefore(point, rewrite.at(id).origin(), WTF::move(division));
            rewrite.edit(id, ValueEdit::Definition).setOperand(operandIndex, quotient);
            rewrite.retype(quotient);
            rewrite.count(StatisticID::IntegerDivisionsReduced);
        });
    });
}

static bool isSameNumber(const Program& program, const Function& function, ValueID a, ValueID b)
{
    if (sameObjectThroughCasts(function, a) == sameObjectThroughCasts(function, b))
        return true;
    KnownValue known = knownValueOf(program, function, a);
    return known.kind() == KnownValue::Kind::Integer && known == knownValueOf(program, function, b);
}

static bool isRemainderOf(const Program& program, const Function& function, ValueID id, const Value& quotient)
{
    const Value& remainder = function.at(sameObjectThroughCasts(function, id));
    return remainder.opcode == Opcode::Mod
        && isSameNumber(program, function, remainder.leftOperand(), quotient.leftOperand())
        && isSameNumber(program, function, remainder.rightOperand(), quotient.rightOperand());
}

AOT_THRESHOLD(unsigned, maxBlocksBetweenRemainderAndQuotient, 4);

bool followsTestForNoRemainder(const Program& program, const Function& function, const CFG& cfg, BlockID block, const Value& quotient)
{
    for (unsigned steps = 0; steps < maxBlocksBetweenRemainderAndQuotient; ++steps) {
        auto incoming = cfg.incomingEdges(block);
        if (incoming.size() != 1 || incoming[0].isHandlerEdge())
            return false;
        Edge edge = incoming[0];
        const Terminator& terminator = function.at(edge.from).terminator;
        if (terminator.kind == TerminatorKind::Branch && !edge.targetIndex) {
            const Value& test = function.at(terminator.conditionOperand());
            if (test.opcode == Opcode::StrictEq) {
                for (unsigned i = 0; i < 2; ++i) {
                    if (knownValueOf(program, function, test.operand(i)) == KnownValue::integer(0) && isRemainderOf(program, function, test.operand(1 - i), quotient))
                        return true;
                }
            }
        }
        block = edge.from;
    }
    return false;
}

// Two integers that leave no remainder have an integer for their quotient, no further from zero than the dividend. A divisor above zero makes no negative zero of a dividend that is zero.
static void reduceQuotientWithoutRemainder(Rewrite& rewrite, const Site& quotient)
{
    const Type& dividend = rewrite.typeOf(quotient.value.leftOperand());
    const Type& divisor = rewrite.typeOf(quotient.value.rightOperand());
    if (!dividend.isInt32() || !divisor.isInt32() || divisor.integers.minimum <= 0 || !followsTestForNoRemainder(rewrite.program, rewrite.function, rewrite.analyses.cfg(), quotient.point.block, quotient.value))
        return;
    rewrite.later([&rewrite, id = quotient.id] {
        Value division = IRBuilder::make(Opcode::DivideIntegers, { rewrite.at(id).leftOperand(), rewrite.at(id).rightOperand() });
        division.setRounding(QuotientRounding::ToInt32);
        rewrite.become(id, WTF::move(division));
        rewrite.count(StatisticID::IntegerDivisionsReduced);
    });
}

static constexpr Rule integerDivisionRules[] {
    { OpcodeSet { Opcode::Div }, reduceQuotientWithoutRemainder },
    { OpcodeSet { Opcode::CallBuiltin }, reduceRoundedQuotient },
    { int32ConvertingOperations, reduceQuotientConvertedToInt32 },
};

PassOutcome reduceIntegerDivisions(FunctionPassContext& context)
{
    return runRules<integerDivisionRules, Rounds::UntilUnchanged>(context);
}

// PropagateBranchConditions.

static bool isAnyUsedTwice(Rewrite& rewrite, const BitVector& values)
{
    BitVector isUsed(rewrite.function.values.size());
    bool isUsedTwice = false;
    auto addUse = [&](ValueID used) {
        if (used && values.quickGet(used.index()))
            isUsedTwice |= isUsed.quickSet(used.index());
    };
    for (BlockID id : rewrite.blocks()) {
        for (ValueID value : rewrite.at(id).values)
            rewrite.at(value).forEachChild(addUse);
        rewrite.at(id).forEachChildOutsideValues(addUse);
        if (isUsedTwice)
            return true;
    }
    return false;
}

static bool canUseValuesOf(Rewrite& rewrite, BlockID block, const Use& use)
{
    if (use.block != block)
        return rewrite.analyses.dominators().terminatorDominates(block, use.block);
    return use.kind != Use::Kind::HandlerArgument && use.kind != Use::Kind::BlockSubject;
}

static bool isConditionOfProvingBranch(Rewrite& rewrite, const Use& use)
{
    return use.kind == Use::Kind::TerminatorOperand && rewrite.at(use.block).terminator.isConditionalBranch()
        && (hasEdgeResults(rewrite.function, Edge { use.block, 0 }) || hasEdgeResults(rewrite.function, Edge { use.block, 1 }));
}

// Whether a value is truthy never changes, so a test that another test of the same value dominates is that test.
static PassOutcome mergeRepeatedTruthinessTests(FunctionPassContext& context)
{
    struct TruthinessTest {
        ValueID tested;
        ValueID test;
        BlockID block;
    };
    if (!context.analyses.opcodesOfValues().contains(Opcode::ToBoolean))
        return PassOutcome::Unchanged;
    Rewrite rewrite(context);
    Vector<TruthinessTest, 16> tests;
    for (BlockID block : rewrite.blocks()) {
        for (ValueID id : rewrite.at(block).values) {
            if (rewrite.at(id).opcode == Opcode::ToBoolean)
                tests.append({ rewrite.at(id).operand(0), id, block });
        }
    }
    if (tests.size() < 2)
        return PassOutcome::Unchanged;
    for (TruthinessTest& test : tests)
        test.tested = rewrite.analyses.identities().identityOf(test.tested);
    AOT::stableSortInLine(tests, { }, &TruthinessTest::tested);

    size_t firstKept = 0;
    size_t endOfKept = 0;
    for (const TruthinessTest& test : tests) {
        if (endOfKept == firstKept || tests[firstKept].tested != test.tested)
            firstKept = endOfKept;
        auto kept = tests.span().subspan(firstKept, endOfKept - firstKept);
        auto dominating = std::ranges::find_if(kept, [&](const TruthinessTest& earlier) {
            return earlier.block == test.block || rewrite.analyses.dominators().terminatorDominates(earlier.block, test.block);
        });
        if (dominating == kept.end()) {
            tests[endOfKept++] = test;
            continue;
        }
        // What a branch proves, it proves of the operand of its condition: of a value that another test has narrowed it hands on more.
        bool testsSameValue = rewrite.at(test.test).operand(0) == rewrite.at(dominating->test).operand(0);
        for (const Use& use : rewrite.uses().usesOf(test.test)) {
            if (testsSameValue || !isConditionOfProvingBranch(rewrite, use))
                rewrite.replaceUse(use, dominating->test);
        }
    }
    return rewrite.commit();
}

// A use that only one edge of a branch on the condition leads to sees what the condition is on that edge.
static void propagateCondition(Rewrite& rewrite, ValueID condition)
{
    struct DecidingEdge {
        Edge edge;
        ValueID constant;
    };
    std::span<const Use> uses = rewrite.uses().usesOf(condition);
    if (uses.size() < 2)
        return;
    Vector<DecidingEdge, 4> edges;
    for (const Use& use : uses) {
        if (use.kind != Use::Kind::TerminatorOperand || rewrite.at(use.block).terminator.kind != TerminatorKind::Branch)
            continue;
        for (Edge edge : { Edge { use.block, 0 }, Edge { use.block, 1 } }) {
            if (rewrite.cfg().singleIncomingEdge(targetOf(rewrite.function, edge).block) == edge)
                edges.append({ edge, ValueID() });
        }
    }
    Vector<std::pair<Use, unsigned>, 4> decidedUses;
    for (const Use& use : uses) {
        if (isConditionOfProvingBranch(rewrite, use))
            continue;
        size_t index = edges.findIf([&](const DecidingEdge& deciding) { return canUseValuesOf(rewrite, targetOf(rewrite.function, deciding.edge).block, use); });
        if (index != notFound)
            decidedUses.append({ use, static_cast<unsigned>(index) });
    }
    if (decidedUses.isEmpty())
        return;
    rewrite.later([&rewrite, condition, edges = WTF::move(edges), decidedUses = WTF::move(decidedUses)]() mutable {
        for (auto& [use, index] : decidedUses) {
            auto& [edge, constant] = edges[index];
            if (!constant) {
                BlockID target = targetOf(rewrite.function, edge).block;
                Value boolean = IRBuilder::make(Opcode::Boolean);
                boolean.setBoolean(!edge.targetIndex);
                constant = rewrite.insertBefore({ target, indexAfterCaughtValues(rewrite.function, rewrite.at(target)) }, rewrite.at(condition).origin(), WTF::move(boolean));
            }
            rewrite.replaceUse(use, constant);
        }
    });
}

// A validating build decides as a release build does, so what it adds counts for nothing.
static bool isCheckOfValidatingBuild(Rewrite& rewrite, const Use& use)
{
    if (use.kind != Use::Kind::Operand)
        return false;
    switch (rewrite.at(use.user).opcode) {
    case Opcode::ValidateType:
    case Opcode::ValidateCallee:
    case Opcode::ValidateLookup:
    case Opcode::ValidateSameValue:
        return true;
    default:
        return false;
    }
}

// A value that is a primitive or an instance of built-in classes, and behind a type test such an instance alone: a use that only the taken edge leads to sees that,
// and the members of those classes are found for it. The front end hands on what `typeof` proves. A test that a pass has made of something else has nobody to do so:
// `value instanceof Error ? value.message : ""` where each object that comes is an error is a test for an object, and behind it the read is one of an error.
static void propagateTestedType(Rewrite& rewrite, BlockID block)
{
    ValueID condition = rewrite.at(block).terminator.conditionOperand();
    // One of two branches on a test may be decided by the other, and a condition that is decided proves nothing.
    if (std::ranges::count(rewrite.uses().usesOf(condition), Use::Kind::TerminatorOperand, &Use::kind) != 1)
        return;
    const Value& test = rewrite.at(condition);
    ValueID tested = test.operand(testedOperandIndex);
    std::span<const Use> uses = rewrite.uses().usesOf(tested);
    if (uses.size() < 2)
        return;
    Edge taken { block, 0 };
    BlockID target = targetOf(rewrite.function, taken).block;
    if (rewrite.cfg().singleIncomingEdge(target) != taken)
        return;
    Type typeOnEdge = narrowOperandByTest({ rewrite.program, rewrite.function, rewrite.context.types }, test, testedOperandIndex, EdgeSide::Taken);
    if (rewrite.typeOf(tested).isObject() || !rewrite.typeOf(tested).hasOnlyClassObjects() || !typeOnEdge.isObject() || !typeOnEdge.isSubsetOf(rewrite.typeOf(tested), rewrite.program))
        return;
    Vector<Use, 4> usesBehindEdge;
    for (const Use& use : uses) {
        if (canUseValuesOf(rewrite, target, use) && !isCheckOfValidatingBuild(rewrite, use))
            usesBehindEdge.append(use);
    }
    if (usesBehindEdge.isEmpty())
        return;
    rewrite.later([&rewrite, taken, usesBehindEdge = WTF::move(usesBehindEdge)] {
        const Target& target = targetOf(rewrite.function, taken);
        size_t existing = target.proofs.find(testedOperandIndex);
        ValueID narrowed = existing != notFound
            ? rewrite.at(target.block).parameters[firstEdgeResultParameter(rewrite.function, taken.from, taken.targetIndex) + existing]
            : rewrite.builder().addEdgeResult(taken, testedOperandIndex);
        for (const Use& use : usesBehindEdge)
            rewrite.replaceUse(use, narrowed);
    });
}

PassOutcome propagateBranchConditions(FunctionPassContext& context)
{
    PassOutcome outcomeOfMerging = mergeRepeatedTruthinessTests(context);
    Rewrite rewrite(context);
    BitVector isVisited;
    // The conditions, and what the type tests among them test.
    BitVector isUndecided(context.function.values.size());
    Vector<ValueID, 16> undecidedConditions;
    Vector<BlockID, 16> branchesOnTypeTests;
    for (BlockID block : rewrite.blocks()) {
        const Terminator& terminator = rewrite.at(block).terminator;
        if (terminator.kind != TerminatorKind::Branch)
            continue;
        const Value& condition = rewrite.at(terminator.conditionOperand());
        if (condition.opcode == Opcode::Boolean || condition.type != TypeTable::boolean)
            continue;
        if (condition.opcode == Opcode::TypeTest) {
            isUndecided.quickSet(condition.operand(testedOperandIndex).index());
            branchesOnTypeTests.append(block);
        }
        if (isVisited.add(terminator.conditionOperand().index())) {
            isUndecided.quickSet(terminator.conditionOperand().index());
            undecidedConditions.append(terminator.conditionOperand());
        }
    }
    if (undecidedConditions.isEmpty() || (!rewrite.analyses.computed().contains(FunctionAnalysis::UseLists) && !isAnyUsedTwice(rewrite, isUndecided)))
        return outcomeOfMerging;
    for (ValueID condition : undecidedConditions)
        propagateCondition(rewrite, condition);
    for (BlockID block : branchesOnTypeTests)
        propagateTestedType(rewrite, block);
    return rewrite.commit() == PassOutcome::Changed ? PassOutcome::Changed : outcomeOfMerging;
}

// SinkConstants.

namespace {

struct ConstantSinker {
    Rewrite& rewrite;
    std::optional<StronglyConnectedComponents> components { };
    Vector<BlockID, 4> userBlocks { };
    Vector<ValueID> blockStamps { };

    bool isCandidate(ValueID id)
    {
        return resultRuleOf(rewrite.at(id).opcode) == ResultRule::Constant && !rewrite.analyses.effectsOf(id).onAnyEdge().interferesWith(Effects::unknownCode());
    }

    bool isOnlyHandedOnOverSomeEdges(ValueID id, BlockID block)
    {
        const UseLists& uses = rewrite.uses();
        if (uses.locationOf(id).placement != ValuePlacement::InBlock || uses.blockOf(id) != block || !isCandidate(id) || rewrite.analyses.hasNoEffects(id))
            return false;
        BitVector targets;
        for (const Use& use : uses.usesOf(id)) {
            if (use.kind != Use::Kind::EdgeArgument || use.block != block)
                return false;
            targets.set(use.targetIndex);
        }
        return targets.bitCount() < rewrite.at(block).terminator.targets.size();
    }

    // A constant that nothing but the terminator of its own block uses, and that goes over some of the edges only, is created on those edges.
    void splitEdgesThatHandOnConstants()
    {
        Vector<Edge, 4> edges;
        for (BlockID block : rewrite.blocks()) {
            const Terminator& terminator = rewrite.at(block).terminator;
            for (uint32_t targetIndex = 0; terminator.targets.size() >= 2 && targetIndex < terminator.targets.size(); ++targetIndex) {
                bool handsOnConstant = std::ranges::any_of(terminator.targets[targetIndex].arguments, [&](ValueID argument) {
                    return resultRuleOf(rewrite.at(argument).opcode) == ResultRule::Constant && isOnlyHandedOnOverSomeEdges(argument, block);
                });
                if (handsOnConstant)
                    edges.append(Edge { block, targetIndex });
            }
        }
        for (Edge edge : edges)
            rewrite.builder().splitEdge(edge);
    }

    // What a block hands to its handler, or is entered with, has to be there at its head. An immediate that a terminator names costs nothing wherever it is defined.
    bool canTakeCopy(const Use& use, ValueID id)
    {
        return use.kind == Use::Kind::Operand || ((use.kind == Use::Kind::TerminatorOperand || use.kind == Use::Kind::EdgeArgument) && !rewrite.analyses.hasNoEffects(id));
    }

    bool canSink(ValueID id, BlockID definitionBlock, std::span<const Use> uses)
    {
        userBlocks.shrink(0);
        for (const Use& use : uses) {
            if (!canTakeCopy(use, id) || use.block == definitionBlock)
                return false;
            if (rewrite.analyses.isInCycle(use.block) && !rewrite.analyses.blockCycles().isInEveryCycleOf(definitionBlock, use.block))
                return false;
            if (userBlocks.isEmpty() || userBlocks.last() != use.block)
                userBlocks.append(use.block);
        }
        return userBlocks.size() == 1 || (!isRematerializable(rewrite.at(id)) && hasExclusiveUserBlocks(id));
    }

    bool hasExclusiveUserBlocks(ValueID candidate)
    {
        const CFG& cfg = rewrite.cfg();
        if (!components)
            components = computeStronglyConnectedComponents(cfg.numNodes(), [&](unsigned index) { return cfg.successors(BlockID(index)); });
        const Vector<uint32_t>& componentOf = components->componentOf;
        if (blockStamps.isEmpty())
            blockStamps.fill(ValueID(), rewrite.function.blocks.size());
        uint32_t minComponent = std::numeric_limits<uint32_t>::max();
        Vector<BlockID, 16> worklist;
        for (BlockID block : userBlocks) {
            minComponent = std::min(minComponent, componentOf[block.index()]);
            worklist.append(block);
        }
        while (!worklist.isEmpty()) {
            for (BlockID target : cfg.successors(worklist.takeLast())) {
                if (componentOf[target.index()] < minComponent || blockStamps[target.index()] == candidate)
                    continue;
                blockStamps[target.index()] = candidate;
                worklist.append(target);
            }
        }
        return !userBlocks.containsIf([&](BlockID block) { return blockStamps[block.index()] == candidate; });
    }

    // Where the users lie in a cycle or reach each other, one value serves them all: it is created in the lowest block outside of every cycle that dominates them.
    std::optional<ProgramPoint> findPointThatServesAll(ValueID original, BlockID definitionBlock, std::span<const Use> uses)
    {
        if (isRematerializable(rewrite.at(original)) || rewrite.analyses.isInCycle(definitionBlock))
            return std::nullopt;
        const Dominators& dominators = rewrite.analyses.dominators();
        BlockID target;
        for (const Use& use : uses) {
            if (!canTakeCopy(use, original) || use.block == definitionBlock)
                return std::nullopt;
            target = target ? dominators.nearestCommonDominator(target, use.block) : use.block;
        }
        // What a block with a handler creates is not there yet when the handler is entered.
        auto servesEveryUser = [&](BlockID block) {
            return !rewrite.analyses.isInCycle(block) && std::ranges::all_of(uses, [&](const Use& use) { return use.block == block || dominators.terminatorDominates(block, use.block); });
        };
        while (target != definitionBlock && !servesEveryUser(target))
            target = dominators.idom(target);
        if (target == definitionBlock)
            return std::nullopt;
        auto firstUseInTarget = std::ranges::find_if(uses, [&](const Use& use) { return use.block == target; });
        return firstUseInTarget != uses.end() ? rewrite.uses().pointOf(*firstUseInTarget) : pointBeforeTerminatorOf(rewrite.function, target);
    }

    void copyLater(BlockID block, ValueID original, ProgramPoint point, std::span<const Use> uses)
    {
        rewrite.later([&rewrite = rewrite, block, original, point, uses = rewrite.keep(uses)] {
            ValueID copy = rewrite.insertBefore(point, rewrite.at(original).origin(), Value(rewrite.at(original)));
            for (const Use& use : uses)
                rewrite.replaceUse(use, copy);
            rewrite.erase(block, original);
        });
    }
};

} // anonymous namespace

PassOutcome sinkConstants(FunctionPassContext& context)
{
    if (context.function.blocks.size() < 2)
        return PassOutcome::Unchanged;
    Rewrite rewrite(context);
    ConstantSinker sinker { rewrite };
    sinker.splitEdgesThatHandOnConstants();
    const UseLists& useLists = rewrite.uses();
    for (BlockID block : rewrite.blocks()) {
        for (ValueID id : rewrite.at(block).values) {
            std::span<const Use> uses = sinker.isCandidate(id) ? useLists.usesOf(id) : std::span<const Use> { };
            if (uses.empty())
                continue;
            if (!sinker.canSink(id, block, uses)) {
                if (std::optional<ProgramPoint> point = sinker.findPointThatServesAll(id, block, uses))
                    sinker.copyLater(block, id, *point, uses);
                continue;
            }
            while (!uses.empty()) {
                size_t numberOfUses = std::ranges::find_if(uses, [&](const Use& use) { return use.block != uses.front().block; }) - uses.begin();
                sinker.copyLater(block, id, useLists.pointOf(uses.front()), uses.first(numberOfUses));
                uses = uses.subspan(numberOfUses);
            }
        }
    }
    return rewrite.commit();
}

// HoistTestsIntoPredecessors.

using TypePredicate = bool (Type::*)() const;

static bool canHoistIntoPredecessors(Rewrite& rewrite, BlockID mergePoint, size_t parameterIndex, NOESCAPE const Invocable<bool(const Type&)> auto& isCheap)
{
    if (rewrite.at(mergePoint).isHandlerBlock())
        return false;
    bool hasCheapArgument = false;
    for (Edge edge : rewrite.cfg().incomingEdges(mergePoint)) {
        if (edge.isHandlerEdge() || edge.from == mergePoint || rewrite.at(edge.from).terminator.kind != TerminatorKind::Jump)
            return false;
        hasCheapArgument |= std::invoke(isCheap, rewrite.typeOf(targetOf(rewrite.function, edge).arguments[parameterIndex]));
    }
    return hasCheapArgument && !rewrite.analyses.isEnteredFromUnreachableBlock(mergePoint);
}

static void hoistTruthinessTest(Rewrite& rewrite, BlockID id, const Block& block)
{
    if (id == entryBlock || block.values.size() != 1 || block.terminator.conditionOperand() != block.values[0])
        return;
    ValueID test = block.values[0];
    ValueID parameter = rewrite.at(test).opcode == Opcode::ToBoolean ? rewrite.at(test).valueOperand() : ValueID();
    size_t parameterIndex = parameter ? block.parameters.find(parameter) : notFound;
    if (parameterIndex == notFound || !canHoistIntoPredecessors(rewrite, id, parameterIndex, &Type::isBoolean))
        return;
    if (rewrite.uses().useCount(parameter) != 1 || rewrite.uses().useCount(test) != 1 || isConditionOfBranchWithProofs(rewrite.function, rewrite.uses(), test) || !rewrite.analyses.isRemovableWhenUnused(test))
        return;
    rewrite.claim(id);
    rewrite.later([&rewrite, id, test, parameter, parameterIndex, edges = rewrite.keep(rewrite.cfg().incomingEdges(id))] {
        for (Edge edge : edges) {
            ValueID argument = targetOf(rewrite.function, edge).arguments[parameterIndex];
            if (!rewrite.typeOf(argument).isBoolean())
                editTarget(rewrite.function, edge).arguments[parameterIndex] = rewrite.insertBefore(pointBeforeTerminatorOf(rewrite.function, edge.from), rewrite.at(test).origin(), IRBuilder::make(Opcode::ToBoolean, { argument }));
        }
        rewrite.edit(id, BlockEdit::Terminator).terminator.setConditionOperand(parameter);
        rewrite.erase(id, test);
        rewrite.retype(parameter);
    });
}

// The first operator of a block, behind its constants, on a parameter that is cheap to work on in some predecessor.
static void hoistOperatorOnParameter(Rewrite& rewrite, BlockID id, const Block& block)
{
    if (id == entryBlock)
        return;
    size_t valueIndex = block.values.findIf([&](ValueID value) { return !isRematerializable(rewrite.at(value)); });
    if (valueIndex == notFound || !operatorsHoistedIntoPredecessors.contains(rewrite.at(block.values[valueIndex]).opcode))
        return;
    ValueID operation = block.values[valueIndex];
    const Value& value = rewrite.at(operation);
    size_t leftIndex = block.parameters.find(value.operand(0));
    size_t rightIndex = block.parameters.find(value.operand(1));
    if ((leftIndex == notFound) == (rightIndex == notFound))
        return;
    uint32_t operandIndex = leftIndex == notFound ? 1 : 0;
    uint32_t parameterIndex = operandIndex ? rightIndex : leftIndex;
    const Type& parameter = rewrite.typeOf(value.operand(operandIndex));
    TypePredicate isCheap = nullptr;
    if (relationalOperators.contains(value.opcode) && !parameter.isNumber())
        isCheap = &Type::isNumber;
    else if (operatorsOnInt32.contains(value.opcode) && parameter.isNumber() && !parameter.isInteger())
        isCheap = &Type::isInteger;
    if (!isCheap || !canHoistIntoPredecessors(rewrite, id, parameterIndex, isCheap))
        return;
    std::span<const Edge> incomingEdges = rewrite.cfg().incomingEdges(id);
    if (!std::ranges::all_of(incomingEdges, [&](Edge edge) { return rewrite.at(edge.from).handler == block.handler; }))
        return;
    if (rewrite.uses().useCount(value.operand(operandIndex)) != 1 || isConditionOfBranchWithProofs(rewrite.function, rewrite.uses(), operation))
        return;
    bool otherOperandIsConstantOfBlock = std::ranges::contains(block.values.span().first(valueIndex), value.operand(1 - operandIndex));
    rewrite.later([&rewrite, id, operation, operandIndex, parameterIndex, otherOperandIsConstantOfBlock, edges = rewrite.keep(incomingEdges)] {
        Opcode opcode = rewrite.at(operation).opcode;
        Origin origin = rewrite.at(operation).origin();
        std::array<ValueID, 2> operands { rewrite.at(operation).operand(0), rewrite.at(operation).operand(1) };
        ValueID otherOperand = operands[1 - operandIndex];
        rewrite.builder().setOrigin(origin);
        ValueID result = rewrite.builder().addParameter(id, rewrite.at(operation).type);
        for (Edge edge : edges) {
            ProgramPoint point = pointBeforeTerminatorOf(rewrite.function, edge.from);
            operands[operandIndex] = targetOf(rewrite.function, edge).arguments[parameterIndex];
            if (otherOperandIsConstantOfBlock)
                operands[1 - operandIndex] = rewrite.insertBefore(point, origin, Value(rewrite.at(otherOperand)));
            rewrite.builder().appendArgument(edge, rewrite.insertBefore(point, origin, IRBuilder::make(opcode, { operands[0], operands[1] })));
        }
        rewrite.replaceUsesOf(operation, result);
        rewrite.erase(id, operation);
        rewrite.removeParameter(id, parameterIndex);
    });
}

// `target.add(round)` behind the choice among what `add` may be: the declared type of the result is expected of what the ways hand on together.
// A way that proves the type needs no guard, and a test in place of the guard asks the other ways alone.
static void hoistGuardOnParameter(Rewrite& rewrite, BlockID id, const Block& block)
{
    if (id == entryBlock)
        return;
    size_t valueIndex = block.values.findIf([&](ValueID value) { return !isRematerializable(rewrite.at(value)); });
    if (valueIndex == notFound || rewrite.at(block.values[valueIndex]).opcode != Opcode::Expect)
        return;
    ValueID guard = block.values[valueIndex];
    ValueID parameter = rewrite.at(guard).valueOperand();
    size_t parameterIndex = block.parameters.find(parameter);
    if (parameterIndex == notFound || rewrite.uses().useCount(parameter) != 1)
        return;
    const Type& expected = rewrite.context.types.at(rewrite.at(guard).expectedType().predicted());
    auto needsNoGuard = [&](const Type& type) { return type.isSubsetOf(expected, rewrite.program); };
    if (!canHoistIntoPredecessors(rewrite, id, parameterIndex, needsNoGuard))
        return;
    rewrite.claim(id);
    rewrite.later([&rewrite, id, guard, parameter, parameterIndex, edges = rewrite.keep(rewrite.cfg().incomingEdges(id))] {
        const Type& expected = rewrite.context.types.at(rewrite.at(guard).expectedType().predicted());
        for (Edge edge : edges) {
            ValueID argument = targetOf(rewrite.function, edge).arguments[parameterIndex];
            if (rewrite.typeOf(argument).isSubsetOf(expected, rewrite.program))
                continue;
            Value copy(rewrite.at(guard));
            copy.setValueOperand(argument);
            editTarget(rewrite.function, edge).arguments[parameterIndex] = rewrite.insertBefore(pointBeforeTerminatorOf(rewrite.function, edge.from), rewrite.at(guard).origin(), WTF::move(copy));
        }
        rewrite.replaceUsesOf(guard, parameter);
        rewrite.erase(id, guard);
        rewrite.retype(parameter);
    });
}

static constexpr Rule hoistedTestRules[] {
    { TerminatorKindSet { TerminatorKind::Branch }, hoistTruthinessTest, BlocksShown::WithParameters },
    { allTerminators, hoistOperatorOnParameter, BlocksShown::WithParameters },
    { allTerminators, hoistGuardOnParameter, BlocksShown::WithParameters },
};

PassOutcome hoistTestsIntoPredecessors(FunctionPassContext& context)
{
    return runRules<hoistedTestRules, Rounds::UntilUnchanged>(context);
}

// RemoveRedundantHandlers.

// An operation whose allocation fails throws when it returns.
static void removeUnusedHandler(Rewrite& rewrite, BlockID id, const Block& block)
{
    auto needsHandler = [](const EffectsOnEdges& effects) {
        return effects.mayThrow() || effects.onAnyEdge().mayCollect;
    };
    if (std::ranges::any_of(block.values, [&](ValueID value) { return needsHandler(rewrite.analyses.effectsOf(value)); }) || needsHandler(rewrite.analyses.effectsOfTerminator(id)))
        return;
    rewrite.later([&rewrite, id] { rewrite.builder().removeHandler(id); });
}

static bool takesValueAlone(const Function& function, const Block& handler)
{
    return handler.role == BlockRole::CatchHandler && std::ranges::none_of(handler.values.span().first(indexAfterCaughtValues(function, handler)), [&](ValueID value) {
        return function.at(value).opcode == Opcode::CatchException;
    });
}

static bool onlyRethrows(Rewrite& rewrite, BlockID id, const Block& block)
{
    if (!block.isHandlerBlock() || !block.parameters.isEmpty() || block.values.isEmpty())
        return false;
    ValueID caught = block.values[0];
    const Terminator& terminator = block.terminator;
    // A `catch` of the program that throws the value again begins another throw, which has frames of its own. Nobody lists them where a `catch` of the function takes the value and ends that throw.
    auto beginsThrowWhoseFramesMayBeListed = [&] {
        return block.role == BlockRole::CatchHandler && !(block.handler && takesValueAlone(rewrite.function, rewrite.at(block.handler->block)));
    };
    bool rethrowsCaughtValue = (rewrite.at(caught).opcode == Opcode::CatchValue && terminator.kind == TerminatorKind::Throw && terminator.valueOperand() == caught && !beginsThrowWhoseFramesMayBeListed())
        || (rewrite.at(caught).opcode == Opcode::CatchException && terminator.kind == TerminatorKind::Rethrow && terminator.exceptionOperand() == caught);
    if (!rethrowsCaughtValue || !std::ranges::all_of(block.values.span().subspan(1), [&](ValueID value) { return rewrite.analyses.isRemovableWhenUnused(value); }))
        return false;
    return std::ranges::none_of(block.values, [&](ValueID value) {
        return (block.handler && block.handler->arguments.contains(value)) || std::ranges::any_of(rewrite.uses().usesOf(value), [&](const Use& use) { return use.block != id; });
    });
}

// Whoever is covered by a handler that only throws on is covered by the handler of that handler.
static void skipRethrowingHandler(Rewrite& rewrite, BlockID id, const Block& block)
{
    if (!onlyRethrows(rewrite, id, block) || (block.handler && block.handler->block == id))
        return;
    for (Edge edge : rewrite.cfg().incomingEdges(id)) {
        if (edge.from == id)
            continue;
        rewrite.later([&rewrite, from = edge.from, outer = std::optional<Target>(block.handler)] {
            if (!rewrite.at(from).handler)
                return;
            if (outer)
                rewrite.edit(from, { BlockEdit::Edges, BlockEdit::Arguments }).handler = *outer;
            else
                rewrite.builder().removeHandler(from);
        });
    }
}

static constexpr Rule redundantHandlerRules[] {
    { allTerminators, removeUnusedHandler, BlocksShown::WithHandler },
    { TerminatorKindSet { TerminatorKind::Throw, TerminatorKind::Rethrow }, skipRethrowingHandler },
};

static bool isRare(const Block& block)
{
    return block.frequency.predicted() == BlockFrequency::Rare;
}

static bool isPollOfLoop(const Function& function, ValueID id)
{
    return function.at(id).opcode == Opcode::LoopHint && !function.at(id).throwsOwedError();
}

static bool likelyIterationsDoNotAllocate(FunctionAnalyses& analyses, const NaturalLoop& loop)
{
    const Function& function = analyses.function();
    for (unsigned index = 0; index < loop.size(); ++index) {
        const Block& block = function.at(loop.at(index));
        if (isRare(block))
            continue;
        for (ValueID value : block.values) {
            if (!isPollOfLoop(function, value) && analyses.effectsOf(value).onAnyEdge().mayCollect)
                return false;
        }
        if (analyses.effectsOfTerminator(loop.at(index)).onAnyEdge().mayCollect)
            return false;
    }
    return true;
}

// A store that adds a field does not throw when its allocation fails: a poll under the handler throws for it, and in a loop that may be the loop's poll.
static bool addsFields(const Function& function, const NaturalLoop& loop)
{
    for (unsigned index = 0; index < loop.size(); ++index) {
        for (ValueID id : function.at(loop.at(index)).values) {
            const Value& value = function.at(id);
            if (value.opcode == Opcode::PutSlotOrAdd || (value.opcode == Opcode::LoopHint && value.throwsOwedError()))
                return true;
            if (value.opcode == Opcode::PutSlots && std::ranges::any_of(value.placedKeys(), &PlacedKey::mayAdd))
                return true;
        }
    }
    return false;
}

static bool needsHandler(const EffectsOnEdges& effects)
{
    return effects.mayThrow() || effects.onAnyEdge().mayCollect;
}

// The polls that throw an owed error before it reaches a loop poll outside its handler.
struct OwedErrorPolls {
    struct Preheader {
        Edge entry;
        Target handler;
    };
    Vector<ProgramPoint, 4> points;
    Vector<Preheader, 1> preheaders;
};

// After the last operation of the block that may leave an error owed, unless a poll that stays under the handler already follows it.
// A fused test stays the last value of its block.
static ProgramPoint pointBeforeBranchOf(const Function& function, BlockID id)
{
    ProgramPoint point = pointBeforeTerminatorOf(function, id);
    if (function.at(id).terminator.kind == TerminatorKind::TestAndBranch)
        --point.index;
    return point;
}

static bool branchNeedsHandler(FunctionAnalyses& analyses, BlockID id)
{
    const Terminator& terminator = analyses.function().at(id).terminator;
    return needsHandler(analyses.effectsOfTerminator(id)) || (terminator.kind == TerminatorKind::TestAndBranch && needsHandler(analyses.effectsOf(terminator.conditionOperand())));
}

static std::optional<ProgramPoint> findPollAfterOperations(FunctionAnalyses& analyses, BlockID id)
{
    const Function& function = analyses.function();
    const Block& block = function.at(id);
    size_t last = block.values.reverseFindIf([&](ValueID value) {
        return !isPollOfLoop(function, value) && needsHandler(analyses.effectsOf(value));
    });
    if (last == notFound || function.at(block.values[last]).opcode == Opcode::LoopHint)
        return std::nullopt;
    return ProgramPoint { id, static_cast<uint32_t>(last + 1) };
}

static bool pollOnRarePaths(FunctionAnalyses& analyses, const NaturalLoop& loop, OwedErrorPolls& polls)
{
    const Function& function = analyses.function();
    for (unsigned index = 0; index < loop.size(); ++index) {
        BlockID id = loop.at(index);
        const Block& block = function.at(id);
        if (!isRare(block))
            continue;
        // What is thrown inside the loop arrives in a handler block of the loop, which polls for itself.
        if (block.terminator.kind == TerminatorKind::Throw || block.terminator.kind == TerminatorKind::Rethrow)
            continue;
        if (branchNeedsHandler(analyses, id))
            return false;
        std::optional<ProgramPoint> point = findPollAfterOperations(analyses, id);
        if (!point)
            continue;
        if (!block.handler)
            return false;
        polls.points.append(*point);
    }
    return true;
}

// An edge from the loop entry reaches the handler without passing the header, so what is defined from the header on must not be read after the handler.
static bool canReachHandlerFromEntry(FunctionAnalyses& analyses, BlockID entry, BlockID header, BlockID handler)
{
    const Function& function = analyses.function();
    Dominators& dominators = analyses.dominators();
    if (dominators.dominates(dominators.idom(handler), entry))
        return true;
    if (!dominators.dominates(header, handler))
        return false;
    BitVector reachedFromHandler(function.blocks.size());
    Vector<BlockID, 16> worklist { handler };
    reachedFromHandler.set(handler.index());
    while (!worklist.isEmpty()) {
        for (BlockID successor : analyses.cfg().successors(worklist.takeLast())) {
            if (successor != header && !reachedFromHandler.set(successor.index()))
                worklist.append(successor);
        }
    }
    auto isReadAfterHandler = [&](ValueID id) {
        return std::ranges::any_of(analyses.useLists().usesOf(id), [&](const Use& use) { return reachedFromHandler.get(use.block.index()); });
    };
    for (BlockID block = dominators.idom(handler);; block = dominators.idom(block)) {
        if (std::ranges::any_of(function.at(block).parameters, isReadAfterHandler) || std::ranges::any_of(function.at(block).values, isReadAfterHandler))
            return false;
        if (block == header)
            return true;
    }
}

// A poll on the loop entry under the handler of the loop poll stands for that poll in the first iteration.
static bool pollOnEntry(FunctionAnalyses& analyses, const NaturalLoop& loop, const Target& handlerOfPoll, OwedErrorPolls& polls)
{
    const Function& function = analyses.function();
    std::optional<Edge> entry = analyses.naturalLoops().findSingleEntry(loop);
    if (!entry || branchNeedsHandler(analyses, entry->from))
        return false;
    const Block& from = function.at(entry->from);
    const Block& header = function.at(loop.header());
    if (from.handler) {
        ProgramPoint end = pointBeforeBranchOf(function, entry->from);
        bool endsWithPoll = end.index && function.at(from.values[end.index - 1]).opcode == Opcode::LoopHint && !isPollOfLoop(function, from.values[end.index - 1]);
        if (!endsWithPoll)
            polls.points.append(end);
        return true;
    }
    if (!canReachHandlerFromEntry(analyses, entry->from, loop.header(), handlerOfPoll.block))
        return false;
    Target handler = handlerOfPoll;
    const Target& target = targetOf(function, *entry);
    if (target.arguments.size() != header.parameters.size())
        return false;
    ProgramPoint endOfEntry = pointBeforeTerminatorOf(function, entry->from);
    for (ValueID& argument : handler.arguments) {
        size_t parameter = header.parameters.find(argument);
        if (parameter != notFound)
            argument = target.arguments[parameter];
        else if (!analyses.dominates(argument, endOfEntry))
            return false;
    }
    polls.preheaders.append({ *entry, WTF::move(handler) });
    return true;
}

// Outside the handler, the back end treats the loop as it would outside a try block. A poll throws the out-of-memory error that an operation has left owed because it could not throw it:
// a store that adds a field, a catch, or a call whose callee caught what it threw. A poll that has left its handler leaves that error owed (operationAOTPollSlow),
// wherever the back end moves it. So that the error is thrown soon, a loop poll only leaves its handler where the likely iterations do not allocate, and a poll under a handler runs
// on the loop entry and after what may allocate on the rare paths. Selection hoists what allocates out of some loops, so it asks again.
static bool removeLoopPollsFromHandlers(FunctionPassContext& context)
{
    Function& function = context.function;
    FunctionAnalyses& analyses = context.analyses;
    if (!analyses.cfg().hasCycle())
        return false;
    struct Poll {
        ProgramPoint point;
        bool restNeedsHandler;
    };
    Vector<Poll, 4> polls;
    OwedErrorPolls owedErrorPolls;
    const NaturalLoops& loops = analyses.naturalLoops();
    Vector<unsigned, 8> outermostFirst;
    for (unsigned index = 0; index < loops.numLoops(); ++index)
        outermostFirst.append(index);
    AOT::sortInLine(outermostFirst, [&](unsigned a, unsigned b) {
        unsigned depthOfA = loops.loopDepth(loops.loop(a).header());
        unsigned depthOfB = loops.loopDepth(loops.loop(b).header());
        return depthOfA != depthOfB ? depthOfA < depthOfB : a < b;
    });
    BitVector loopsWhereNothingIsOwed(loops.numLoops());
    for (unsigned loopIndex : outermostFirst) {
        const NaturalLoop& loop = loops.loop(loopIndex);
        std::optional<bool> canRemovePoll;
        for (unsigned index = 0; index < loop.size(); ++index) {
            BlockID id = loop.at(index);
            const Block& block = function.at(id);
            if (!block.handler || loops.innerMostLoopOf(id) != &loop)
                continue;
            size_t position = block.values.findIf([&](ValueID value) { return needsHandler(analyses.effectsOf(value)); });
            if (position == notFound || !isPollOfLoop(function, block.values[position]))
                continue;
            if (!canRemovePoll) {
                const NaturalLoop* outer = loops.innerMostOuterLoop(loop);
                canRemovePoll = outer && loopsWhereNothingIsOwed.get(outer->index());
                if (!*canRemovePoll) {
                    OwedErrorPolls added;
                    canRemovePoll = id == loop.header() && likelyIterationsDoNotAllocate(analyses, loop) && !addsFields(function, loop) && pollOnRarePaths(analyses, loop, added) && pollOnEntry(analyses, loop, *block.handler, added);
                    if (*canRemovePoll) {
                        for (ProgramPoint point : added.points)
                            owedErrorPolls.points.appendIfNotContains(point);
                        owedErrorPolls.preheaders.appendVector(WTF::move(added.preheaders));
                    }
                }
                if (*canRemovePoll)
                    loopsWhereNothingIsOwed.set(loopIndex);
            }
            if (!*canRemovePoll)
                break;
            bool restNeedsHandler = needsHandler(analyses.effectsOfTerminator(id)) || std::ranges::any_of(block.values.span().subspan(position + 1), [&](ValueID value) { return needsHandler(analyses.effectsOf(value)); });
            polls.append({ { id, static_cast<uint32_t>(position) }, restNeedsHandler });
        }
    }
    if (polls.isEmpty())
        return false;

    IRBuilder builder(analyses.context(), function, context.types, &analyses);
    auto insertPoll = [&](ProgramPoint point) {
        const Block& block = function.at(point.block);
        Value poll = IRBuilder::make(Opcode::LoopHint);
        poll.setThrowsOwedError(true);
        builder.setOrigin(point.index ? function.at(block.values[point.index - 1]).origin() : block.terminator.origin());
        builder.insertBefore(point, WTF::move(poll));
    };
    for (ProgramPoint point : owedErrorPolls.points) {
        for (Poll& poll : polls)
            poll.restNeedsHandler |= poll.point.block == point.block;
        insertPoll(point);
    }
    builder.applyInsertions();
    for (OwedErrorPolls::Preheader& preheader : owedErrorPolls.preheaders) {
        builder.setOrigin(function.at(preheader.entry.from).terminator.origin());
        insertPoll(ProgramPoint::atHeadOf(builder.splitEdge(preheader.entry, WTF::move(preheader.handler))));
        builder.applyInsertions();
    }
    for (const Poll& poll : polls) {
        if (poll.restNeedsHandler) {
            builder.setOrigin(function.at(function.at(poll.point.block).values[poll.point.index]).origin());
            builder.splitBlock({ poll.point.block, poll.point.index + 1 });
        }
        builder.removeHandler(poll.point.block);
        function.edit(function.at(poll.point.block).values[poll.point.index], ValueEdit::Definition).setLeavesOwedError(true);
    }
    return true;
}

// A store that adds a field allocates and may not collect, so it does not throw when its allocation fails. Under a handler, a poll after the last such store of a block throws for them.
static bool pollAfterFieldAddingStores(FunctionPassContext& context)
{
    Function& function = context.function;
    if (!context.analyses.opcodesOfValues().contains(Opcode::PutSlotOrAdd))
        return false;
    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);
    bool hasChanged = false;
    for (BlockID id : context.analyses.cfg().reversePostOrder()) {
        const Block& block = function.at(id);
        if (!block.handler)
            continue;
        size_t last = block.values.reverseFindIf([&](ValueID value) { return function.at(value).opcode == Opcode::PutSlotOrAdd; });
        if (last == notFound)
            continue;
        if (std::ranges::any_of(block.values.span().subspan(last + 1), [&](ValueID value) { return function.at(value).opcode == Opcode::LoopHint; }))
            continue;
        Value poll = IRBuilder::make(Opcode::LoopHint);
        poll.setThrowsOwedError(true);
        builder.setOrigin(function.at(block.values[last]).origin());
        builder.insertBefore({ id, static_cast<uint32_t>(last + 1) }, WTF::move(poll));
        hasChanged = true;
    }
    builder.applyInsertions();
    return hasChanged;
}

static PassOutcome removeRedundantHandlers(FunctionPassContext& context, bool hasAddedPolls)
{
    if (runRules<redundantHandlerRules, Rounds::UntilUnchanged>(context) == PassOutcome::Unchanged)
        return hasAddedPolls ? PassOutcome::Changed : PassOutcome::Unchanged;
    Rewrite(context).builder().emptyUnreachableBlocks();
    return PassOutcome::Changed;
}

PassOutcome removeRedundantHandlers(FunctionPassContext& context)
{
    if (!context.analyses.cfg().hasHandlers())
        return PassOutcome::Unchanged;
    bool hasAddedPolls = pollAfterFieldAddingStores(context);
    hasAddedPolls |= removeLoopPollsFromHandlers(context);
    return removeRedundantHandlers(context, hasAddedPolls);
}

PassOutcome removeRedundantHandlersAfterSelection(FunctionPassContext& context, bool removedAllocationUnderHandler)
{
    if (!context.analyses.cfg().hasHandlers())
        return PassOutcome::Unchanged;
    bool hasAddedPolls = pollAfterFieldAddingStores(context);
    hasAddedPolls |= removeLoopPollsFromHandlers(context);
    if (!hasAddedPolls && !removedAllocationUnderHandler)
        return PassOutcome::Unchanged;
    return removeRedundantHandlers(context, hasAddedPolls);
}

// TestNarrowerTypesFirst.

static const Value* findTypeTestThatEnds(Rewrite& rewrite, const Block& block)
{
    if (block.terminator.kind != TerminatorKind::Branch || block.values.isEmpty() || block.terminator.conditionOperand() != block.values.last())
        return nullptr;
    const Value& test = rewrite.at(block.values.last());
    return test.opcode == Opcode::TypeTest && rewrite.uses().useCount(block.values.last()) == 1 ? &test : nullptr;
}

static void testNarrowerTypeFirst(Rewrite& rewrite, BlockID id, const Block& block)
{
    if (block.frequency.predicted() == BlockFrequency::Rare || block.handler)
        return;
    const Value* wider = findTypeTestThatEnds(rewrite, block);
    const Target& proven = block.terminator.targets[0];
    if (!wider || !proven.arguments.isEmpty() || proven.proofs.size() != 1 || proven.proofs[0])
        return;

    BlockID next = proven.block;
    const Block& nextBlock = rewrite.at(next);
    if (next == id || nextBlock.handler || nextBlock.isHandlerBlock() || nextBlock.values.size() != 1 || nextBlock.parameters.size() != 1 || !rewrite.cfg().singleIncomingEdge(next))
        return;
    const Value* narrower = findTypeTestThatEnds(rewrite, nextBlock);
    if (!narrower || narrower->valueOperand() != nextBlock.parameters[0] || rewrite.uses().useCount(nextBlock.parameters[0]) != 1)
        return;
    const Target& provenNarrower = nextBlock.terminator.targets[0];
    if (provenNarrower.block == next || provenNarrower.block == id)
        return;
    const Type& widerType = rewrite.context.types.at(wider->testedType());
    const Type& narrowerType = rewrite.context.types.at(narrower->testedType());
    if (wider->testedType() == narrower->testedType() || !narrowerType.isSubsetOf(widerType, rewrite.program))
        return;
    // `links[id] ??= ..`, `session().project`: the second test asks whether there is nothing. Mostly there is something, which would fail it, then pass the first test on the failing side and be asked again.
    if (narrowerType.isUndefinedOrNull() || (rewrite.at(provenNarrower.block).frequency > rewrite.at(nextBlock.terminator.targets[1].block).frequency).predicted())
        return;

    rewrite.claim(id);
    rewrite.claim(next);
    // Where the first test admits nothing beside the second but undefined and null, flow expects `undefined|number` and the use needs a number: the rest is unlikely.
    Expected<BlockFrequency> frequencyOfRest = widerType.withoutNullish().isSubsetOf(narrowerType, rewrite.program) ? BlockFrequency::Unlikely : block.frequency;
    rewrite.later([&rewrite, id, test = block.values.last(), narrowerType = narrower->testedType(), provenNarrower = Target(provenNarrower), frequencyOfRest]() mutable {
        IRBuilder& builder = rewrite.builder();
        Target taken = rewrite.at(id).terminator.targets[0];
        Target notTaken = rewrite.at(id).terminator.targets[1];
        builder.setOrigin(rewrite.at(test).origin());
        BlockID failed = builder.addBlock(frequencyOfRest);
        builder.moveToEndOf(failed);
        ValueID widerTest = builder.appendTypeTest(rewrite.at(test).valueOperand(), rewrite.at(test).testedType());
        builder.emitBranch(widerTest, WTF::move(taken), WTF::move(notTaken));
        rewrite.edit(test, ValueEdit::Definition).setTestedType(narrowerType);
        builder.redirectEdge(Edge { id, 0 }, WTF::move(provenNarrower));
        builder.redirectEdge(Edge { id, 1 }, Target { failed, { }, { } });
    });
}

static constexpr Rule narrowerTestRules[] { { TerminatorKindSet { TerminatorKind::Branch }, testNarrowerTypeFirst } };

static bool handOnWhatComparisonsWithTextsDoNotNarrow(FunctionPassContext& context)
{
    Function& function = context.function;
    bool changed = false;
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        BlockID id(blockIndex);
        if (function.at(id).terminator.kind != TerminatorKind::Branch)
            continue;
        const Value& test = function.at(function.at(id).terminator.conditionOperand());
        if (test.opcode != Opcode::StrictEq || (function.at(test.leftOperand()).opcode != Opcode::String && function.at(test.rightOperand()).opcode != Opcode::String))
            continue;
        for (uint32_t targetIndex = 0; targetIndex < 2; ++targetIndex) {
            const Target& target = function.at(id).terminator.targets[targetIndex];
            if (target.proofs.isEmpty())
                continue;
            uint32_t firstParameter = firstEdgeResultParameter(function, id, targetIndex);
            Vector<ValueID, 2> handedOn;
            for (uint32_t index = 0; index < target.proofs.size(); ++index) {
                ValueID narrowed = operandNarrowedOnEdge(function, { id, targetIndex, index });
                if (!narrowed || function.at(function.at(target.block).parameters[firstParameter + index]).type != function.at(narrowed).type)
                    break;
                handedOn.append(narrowed);
            }
            if (handedOn.size() != target.proofs.size())
                continue;
            Target& edited = function.edit(id, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator.targets[targetIndex];
            edited.arguments.appendVector(handedOn);
            edited.proofs.clear();
            context.analyses.didChangeBlock(id);
            changed = true;
        }
    }
    return changed;
}

PassOutcome testNarrowerTypesFirst(FunctionPassContext& context)
{
    bool changed = handOnWhatComparisonsWithTextsDoNotNarrow(context);
    PassOutcome outcome = runRules<narrowerTestRules>(context);
    return changed ? PassOutcome::Changed : outcome;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
