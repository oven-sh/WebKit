/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValidatorBuilder.h"

#if ENABLE(AOT)

#include "AOTTypeArena.h"
#include "AOTTypeTestSite.h"
#include "AOTValidationPlanText.h"
#include <algorithm>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t outcomeResultIndex = 0;
static constexpr uint32_t valueResultIndex = 1;

static constexpr int64_t interpretedVerdictIndex = 0;
static constexpr int64_t interpretedValueIndex = 1;

static OptionSet<RuntimeClass> classesAcceptedBy(ResultShape shape)
{
    switch (shape) {
    case ResultShape::Unknown:
        return OptionSet<RuntimeClass>::all();
    case ResultShape::Never:
    case ResultShape::Union:
        return { };
    case ResultShape::Undefined:
        return RuntimeClass::Undefined;
    case ResultShape::Null:
        return RuntimeClass::Null;
    case ResultShape::AnyObject:
        return { RuntimeClass::Array, RuntimeClass::Object };
    case ResultShape::String:
    case ResultShape::StringLiteral:
        return RuntimeClass::String;
    case ResultShape::Number:
    case ResultShape::NumberLiteral:
        return RuntimeClass::Number;
    case ResultShape::Boolean:
    case ResultShape::BooleanLiteral:
        return RuntimeClass::Boolean;
    case ResultShape::BigInt:
        return RuntimeClass::BigInt;
    case ResultShape::Array:
    case ResultShape::Tuple:
        return RuntimeClass::Array;
    case ResultShape::Dictionary:
    case ResultShape::Object:
        return RuntimeClass::Object;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

OptionSet<RuntimeClass> runtimeClassesOf(const Program& program, ResultNodeRef ref)
{
    OptionSet<RuntimeClass> classes;
    forEachAlternative(program, ref, [&](const ResultNode& node) {
        classes.add(classesAcceptedBy(node.shape));
        if (node.defaultValue)
            classes.add(RuntimeClass::Undefined);
    });
    return classes;
}

bool appliesDefault(const Program& program, ResultNodeRef ref)
{
    bool hasDefault = false;
    forEachAlternative(program, ref, [&](const ResultNode& node) {
        hasDefault |= node.defaultValue.has_value();
    });
    return hasDefault;
}

static TypeID typeOfPrimitive(TypeArena& types, ResultShape shape)
{
    switch (shape) {
    case ResultShape::String:
        return TypeTable::string;
    case ResultShape::Number:
        return TypeTable::number;
    case ResultShape::Boolean:
        return TypeTable::boolean;
    case ResultShape::BigInt:
        return types.intern(Type::fromPrimitives(Primitive::BigInt));
    case ResultShape::Unknown:
    case ResultShape::Never:
    case ResultShape::Undefined:
    case ResultShape::Null:
    case ResultShape::AnyObject:
    case ResultShape::BooleanLiteral:
    case ResultShape::NumberLiteral:
    case ResultShape::StringLiteral:
    case ResultShape::Array:
    case ResultShape::Tuple:
    case ResultShape::Dictionary:
    case ResultShape::Object:
    case ResultShape::Union:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Value makePlainArrayTest(ValueID value)
{
    return makePristineTest(value, BuiltinClasses::Array, { PristineAspects::Storage, PristineAspects::Names });
}

static Value makeElementRead(ValueID array, ValueID index, ValueID guard)
{
    Value read = IRBuilder::make(Opcode::GetArrayElement, { array, index, guard });
    read.setArrayStorage(ArrayStorage::Other);
    read.setKeyIsIntegral(true);
    return read;
}

// The elements of a proxy of an array are read by the general validator, like those of every array that is not plain.
static void emitProxiedArray(ValidatorBuilder& validator, BlockID notArrayInstance, ValueID value, const ValidationExits& exits)
{
    IRBuilder& builder = validator.builder();
    BlockID resumed = builder.currentBlock();
    builder.moveToEndOf(notArrayInstance);
    validator.emitJumpUnless(validator.appendHelperCall(Builtins::ArrayIsArray, { value }), exits.rejected);
    builder.emitJump(exits.unknown);
    builder.moveToEndOf(resumed);
}

static Vector<ValidationRule, 2> withoutLengthRules(std::span<const ValidationRule> rules)
{
    Vector<ValidationRule, 2> result;
    for (const ValidationRule& rule : rules) {
        if (subjectOf(rule) != RuleSubject::Length)
            result.append(rule);
    }
    return result;
}

ValidatorBuilder::ValidatorBuilder(IRBuilder& builder, const EmissionInputs& inputs)
    : Emission(builder, inputs)
    , m_rules(*this)
{
}

bool ValidatorBuilder::validationEndsStretch(ResultNodeRef ref)
{
    ResultNode node = describeNode(program(), ref);
    if (node.isContainer() || decideNodeCode(decisionInputs(), ref, NodeUse::CopiedFromValue).code == NodeCode::Interpreted)
        return true;
    return std::ranges::any_of(node.rules, [&](const ValidationRule& rule) {
        switch (m_rules.codeFor(rule, node.shape)) {
        case RuleCode::ByHelper:
        case RuleCode::RegularExpression:
            return true;
        case RuleCode::Inline:
        case RuleCode::Nothing:
            return false;
        }
        RELEASE_ASSERT_NOT_REACHED();
    });
}

ValueID ValidatorBuilder::emitNode(ResultNodeRef ref, ValueID value, const ValidationExits& exits)
{
    NodeDecision decision = decideNodeCode(decisionInputs(), ref, NodeUse::CopiedFromValue);
    if (decision.code == NodeCode::Interpreted) {
        addInterpretedNodeDecline(decisionInputs(), firstLayoutOf(program(), ref), decision.cause);
        return emitInterpretedNode(ref, value, exits);
    }

    ResultNode node = describeNode(program(), ref);
    if (node.isContainer())
        return emitCallOfValidator(ref, node, value, exits);
    return emitNodeBody(ref, value, exits);
}

ValueID ValidatorBuilder::emitCallOfValidator(ResultNodeRef ref, const ResultNode&, ValueID value, const ValidationExits& exits)
{
    ValueID results = appendCallOf(inputs().functions.ensure(validatorKeyOf(ref)), { value });
    BlockID parsed = builder().addBlock();
    emitBranchOnOutcome(appendResultAt(results, outcomeResultIndex), parsed, exits.rejected, exits.unknown);
    builder().moveToEndOf(parsed);
    return appendResultAt(results, valueResultIndex);
}

ValueID ValidatorBuilder::emitInterpretedNode(ResultNodeRef ref, ValueID value, const ValidationExits& exits)
{
    StringID plan = validationPlanText(program(), inputs().strings, ref.expectedResult(), ref.part);
    ValueID pair = appendHelperCall(Builtins::CompilerHelpersInterpretNode, { value, appendString(plan) });
    ValueID verdict = builder().append(Opcode::ElemGet, { pair, builder().appendInteger(interpretedVerdictIndex) });
    emitJumpUnless(builder().append(Opcode::ToBoolean, { verdict }), exits.rejected);
    return builder().append(Opcode::ElemGet, { pair, builder().appendInteger(interpretedValueIndex) });
}

ValueID ValidatorBuilder::emitNodeBody(ResultNodeRef ref, ValueID value, const ValidationExits& exits)
{
    ResultNode node = describeNode(program(), ref);
    if (node.defaultValue)
        value = emitDefault(node, value, builder().addBlock());

    ValueID result;
    switch (node.shape) {
    case ResultShape::Unknown:
        result = value;
        break;
    case ResultShape::Never:
        builder().emitJump(exits.rejected);
        builder().moveToEndOf(builder().addBlock(BlockFrequency::Rare));
        result = builder().appendUndefined();
        break;
    case ResultShape::Undefined:
        result = emitTypeTest(value, TypeTable::undefined, exits.rejected);
        break;
    case ResultShape::Null:
        result = emitTypeTest(value, TypeTable::null, exits.rejected);
        break;
    case ResultShape::AnyObject:
        result = emitAnyObject(value, exits);
        break;
    case ResultShape::String:
    case ResultShape::Number:
    case ResultShape::Boolean:
    case ResultShape::BigInt:
        result = emitPrimitive(node, value, exits);
        break;
    case ResultShape::BooleanLiteral:
    case ResultShape::NumberLiteral:
    case ResultShape::StringLiteral:
        result = emitLiteral(node, value, exits);
        break;
    case ResultShape::Array:
        result = emitArray(node, value, exits);
        break;
    case ResultShape::Tuple:
        result = emitTuple(node, value, exits);
        break;
    case ResultShape::Dictionary:
        result = emitDictionary(node, value, exits);
        break;
    case ResultShape::Object:
        result = emitObject(ref, node, value, exits);
        break;
    case ResultShape::Union:
        result = emitUnion(ref, node, value, exits);
        break;
    }

    Vector<ValidationRule, 2> rulesAfterCopy;
    if (node.shape == ResultShape::Array) {
        rulesAfterCopy = withoutLengthRules(node.rules);
        node.rules = rulesAfterCopy.span();
    }
    m_rules.emitRules(node, result, exits.rejected);
    return result;
}

ValueID ValidatorBuilder::emitDefault(const ResultNode& node, ValueID value, BlockID whenDefined)
{
    ValueID isUndefined = builder().appendTypeTest(value, TypeTable::undefined);
    IRBuilder::ProvenBranch branch = builder().emitBranchWithProof(isUndefined, testedOperandIndex, BlockFrequency::Normal, BlockFrequency::Normal);
    ValueID defaultValue = m_rules.appendLiteral(*node.defaultValue);

    const Function& function = builder().function();
    TypeID definedType = inputs().types.unionOf(function.at(defaultValue).type, function.at(branch.provenOnNotTaken).type);
    ValueID defined = builder().addParameter(whenDefined, definedType);
    builder().emitJump(whenDefined, { defaultValue });

    builder().moveToEndOf(branch.notTaken);
    builder().emitJump(whenDefined, { branch.provenOnNotTaken });

    builder().moveToEndOf(whenDefined);
    return defined;
}

ValueID ValidatorBuilder::emitPrimitive(const ResultNode& node, ValueID value, const ValidationExits& exits)
{
    ValueID primitive = emitTypeTest(value, typeOfPrimitive(inputs().types, node.shape), exits.rejected);
    if (node.shape == ResultShape::Number)
        m_rules.emitRequireFinite(primitive, exits.rejected);
    return primitive;
}

ValueID ValidatorBuilder::emitLiteral(const ResultNode& node, ValueID value, const ValidationExits& exits)
{
    ValueID isEqual = builder().append(Opcode::StrictEq, { value, m_rules.appendLiteral(node.literal) });
    return emitProvenBranch(isEqual, exits.rejected);
}

ValueID ValidatorBuilder::emitAnyObject(ValueID value, const ValidationExits& exits)
{
    return emitTypeTest(value, TypeTable::object, exits.rejected);
}

ValueID ValidatorBuilder::emitPlainArray(ValueID array, const ValidationExits& exits)
{
    return emitProvenBranch(builder().append(makePlainArrayTest(array)), exits.unknown);
}

ValueID ValidatorBuilder::emitArray(const ResultNode& node, ValueID value, const ValidationExits& exits)
{
    RELEASE_ASSERT(node.children.size() == 1);
    ResultNodeRef element = node.children[0];
    bool elementEndsStretch = validationEndsStretch(element);

    BlockID notArrayInstance = builder().addBlock(BlockFrequency::Rare);
    ValueID array = emitRequireClass(RuntimeClass::Array, value, notArrayInstance);
    ValueID guard = emitPlainArray(array, exits);
    emitProxiedArray(*this, notArrayInstance, value, exits);

    ValueID length = builder().append(Opcode::GetArrayLength, { array, guard });
    m_rules.emitLengthRules(node, length, exits.rejected);
    ValueID copy = builder().append(makeArrayOf(program(), node, { }));

    BlockID header = builder().addBlock();
    ValueID position = builder().addParameter(header, inputs().types.intern(Type::integer(Range::uint32())));
    Arguments entryArguments { builder().appendInteger(0) };
    if (elementEndsStretch) {
        entryArguments.append(guard);
        guard = builder().addParameter(header, TypeTable::guard);
    }
    builder().emitJump(header, WTF::move(entryArguments));

    builder().moveToEndOf(header);
    builder().append(Opcode::LoopHint, { });
    ValueID isInRange = builder().append(Opcode::Less, { position, length });
    IRBuilder::ProvenBranch inRange = builder().emitBranchWithProof(isInRange, testedOperandIndex, BlockFrequency::Normal, BlockFrequency::Normal);
    ValueID index = inRange.provenOnTaken;

    ValueID validated = emitNode(element, builder().append(makeElementRead(array, index, guard)), exits);
    builder().append(Opcode::AppendElement, { copy, validated });
    Arguments backEdgeArguments { builder().append(Opcode::Add, { index, builder().appendInteger(1) }) };
    if (elementEndsStretch)
        backEdgeArguments.append(emitProvenBranch(builder().append(makePlainArrayTest(array)), exits.unknown));
    builder().emitJump(header, WTF::move(backEdgeArguments));

    builder().moveToEndOf(inRange.notTaken);
    return copy;
}

ValueID ValidatorBuilder::emitTuple(const ResultNode& node, ValueID value, const ValidationExits& exits)
{
    BlockID notArrayInstance = builder().addBlock(BlockFrequency::Rare);
    ValueID array = emitRequireClass(RuntimeClass::Array, value, notArrayInstance);
    ValueID guard = emitPlainArray(array, exits);
    emitProxiedArray(*this, notArrayInstance, value, exits);

    ValueID length = builder().append(Opcode::GetArrayLength, { array, guard });
    ValueID expectedLength = builder().appendInteger(static_cast<int64_t>(node.children.size()));
    emitJumpUnless(builder().append(Opcode::StrictEq, { length, expectedLength }), exits.rejected);

    Vector<ValueID, 4> elements;
    for (size_t index = 0; index < node.children.size(); ++index)
        elements.append(builder().append(makeElementRead(array, builder().appendInteger(static_cast<int64_t>(index)), guard)));
    for (size_t index = 0; index < node.children.size(); ++index)
        elements[index] = emitNode(node.children[index], elements[index], exits);
    return builder().append(makeArrayOf(program(), node, elements.span()));
}

DerivedSignature ValidatorBuilder::signatureOf(Emission& emission, const ResultNode& node)
{
    Type validated = emission.program().at(node.type).withoutClassProofs().withUntestableBuiltinsWidened().withPrimitives(Primitive::Undefined);
    return { { TypeTable::anything }, { emission.outcomeType(), emission.inputs().types.intern(validated) } };
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
