/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSelectTypedResultsNodes.h"

#if ENABLE(AOT)

#include "AOTIRBuilder.h"
#include "AOTSelectTypedResultsTypeNodes.h"
#include "AOTValidationRules.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static void forEachChild(const ValidationShape& shape, NOESCAPE const Invocable<void(uint32_t)> auto& visit)
{
    switch (shape.kind()) {
    case ValidationShape::Kind::Unknown:
    case ValidationShape::Kind::Never:
    case ValidationShape::Kind::Undefined:
    case ValidationShape::Kind::Null:
    case ValidationShape::Kind::AnyObject:
    case ValidationShape::Kind::Primitive:
    case ValidationShape::Kind::BooleanLiteral:
    case ValidationShape::Kind::NumberLiteral:
    case ValidationShape::Kind::StringLiteral:
        return;
    case ValidationShape::Kind::Array:
        visit(shape.as<ValidationShape::Array>().element);
        return;
    case ValidationShape::Kind::Dictionary:
        visit(shape.as<ValidationShape::Dictionary>().value);
        return;
    case ValidationShape::Kind::Tuple:
        for (uint32_t element : shape.as<ValidationShape::Tuple>().elements)
            visit(element);
        return;
    case ValidationShape::Kind::Union:
        for (uint32_t arm : shape.as<ValidationShape::Union>().arms)
            visit(arm);
        return;
    case ValidationShape::Kind::Object: {
        const auto& object = shape.as<ValidationShape::Object>();
        for (const ValidationMember& member : object.members)
            visit(member.node);
        if (object.undeclaredValue)
            visit(*object.undeclaredValue);
        return;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<ResultLiteral> defaultOf(const ValidationRule& rule)
{
    switch (rule.kind()) {
    case ValidationRule::Kind::Integer:
    case ValidationRule::Kind::Minimum:
    case ValidationRule::Kind::Maximum:
    case ValidationRule::Kind::ExclusiveMinimum:
    case ValidationRule::Kind::ExclusiveMaximum:
    case ValidationRule::Kind::MultipleOf:
    case ValidationRule::Kind::MinimumLength:
    case ValidationRule::Kind::MaximumLength:
    case ValidationRule::Kind::Pattern:
    case ValidationRule::Kind::Format:
    case ValidationRule::Kind::Template:
    case ValidationRule::Kind::Message:
        return std::nullopt;
    case ValidationRule::Kind::DefaultNull:
        return ResultLiteral { .kind = ResultLiteral::Kind::Null };
    case ValidationRule::Kind::DefaultBoolean:
        return ResultLiteral { .kind = ResultLiteral::Kind::Boolean, .boolean = rule.as<ValidationRule::DefaultBoolean>().value };
    case ValidationRule::Kind::DefaultNumber:
        return ResultLiteral { .kind = ResultLiteral::Kind::Number, .number = rule.as<ValidationRule::DefaultNumber>().value };
    case ValidationRule::Kind::DefaultString:
        return ResultLiteral { .kind = ResultLiteral::Kind::String, .string = rule.as<ValidationRule::DefaultString>().value };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool hasDefault(const ValidationNode& node)
{
    return node.rules.containsIf([](const ValidationRule& rule) {
        return defaultOf(rule).has_value();
    });
}

static bool acceptsUndefined(const Vector<ValidationNode>& nodes, uint32_t root)
{
    if (hasDefault(nodes[root]))
        return false;

    BitVector visited;
    Vector<uint32_t, 8> alternatives { root };
    while (!alternatives.isEmpty()) {
        const ValidationShape& shape = nodes[alternatives.takeLast()].shape;
        switch (shape.kind()) {
        case ValidationShape::Kind::Unknown:
        case ValidationShape::Kind::Undefined:
            return true;
        case ValidationShape::Kind::Union:
            for (uint32_t arm : shape.as<ValidationShape::Union>().arms) {
                if (arm != root && visited.add(arm) && !hasDefault(nodes[arm]))
                    alternatives.append(arm);
            }
            break;
        case ValidationShape::Kind::Never:
        case ValidationShape::Kind::Null:
        case ValidationShape::Kind::AnyObject:
        case ValidationShape::Kind::Primitive:
        case ValidationShape::Kind::BooleanLiteral:
        case ValidationShape::Kind::NumberLiteral:
        case ValidationShape::Kind::StringLiteral:
        case ValidationShape::Kind::Array:
        case ValidationShape::Kind::Tuple:
        case ValidationShape::Kind::Dictionary:
        case ValidationShape::Kind::Object:
            break;
        }
    }
    return false;
}

static ResultShape shapeOf(ValidatedPrimitive primitive)
{
    switch (primitive) {
    case ValidatedPrimitive::String:
        return ResultShape::String;
    case ValidatedPrimitive::Number:
        return ResultShape::Number;
    case ValidatedPrimitive::Boolean:
        return ResultShape::Boolean;
    case ValidatedPrimitive::BigInt:
        return ResultShape::BigInt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void describeRules(const ValidationNode& source, ResultNode& node)
{
    node.rules = source.rules.span();
    for (const ValidationRule& rule : node.rules) {
        if (!node.defaultValue)
            node.defaultValue = defaultOf(rule);
        if (const auto* message = rule.tryAs<ValidationRule::Message>(); message && !node.message)
            node.message = message->text;
    }
}

static void describeChildren(ExpectedResultID expectedResult, const Vector<uint32_t>& children, ResultNode& node)
{
    node.children.reserveInitialCapacity(children.size());
    for (uint32_t child : children)
        node.children.append(ResultNodeRef::forValidation(expectedResult, child));
}

static void describeObject(const Vector<ValidationNode>& nodes, ExpectedResultID expectedResult, const ValidationShape::Object& object, ResultNode& node)
{
    node.layout = object.layout;
    node.undeclared = object.undeclared;
    if (object.undeclaredValue)
        node.undeclaredValue = ResultNodeRef::forValidation(expectedResult, *object.undeclaredValue);
    node.members.reserveInitialCapacity(object.members.size());
    for (const ValidationMember& member : object.members) {
        node.members.append(ResultMember {
            .key = member.key,
            .node = ResultNodeRef::forValidation(expectedResult, member.node),
            .field = member.field,
            .isOptional = member.isOptional,
            .mustBeOwn = !member.isOptional && acceptsUndefined(nodes, member.node),
        });
    }
}

static ResultNode describeValidationNode(const Program& program, ExpectedResultID expectedResult, uint32_t part)
{
    const Vector<ValidationNode>& nodes = program.at(expectedResult).validation;
    const ValidationNode& source = nodes[part];
    const ValidationShape& shape = source.shape;

    ResultNode node;
    node.type = source.type;
    node.text = source.text;
    describeRules(source, node);

    switch (shape.kind()) {
    case ValidationShape::Kind::Unknown:
        node.shape = ResultShape::Unknown;
        return node;
    case ValidationShape::Kind::Never:
        node.shape = ResultShape::Never;
        return node;
    case ValidationShape::Kind::Undefined:
        node.shape = ResultShape::Undefined;
        return node;
    case ValidationShape::Kind::Null:
        node.shape = ResultShape::Null;
        return node;
    case ValidationShape::Kind::AnyObject:
        node.shape = ResultShape::AnyObject;
        return node;
    case ValidationShape::Kind::Primitive:
        node.shape = shapeOf(shape.as<ValidationShape::Primitive>().primitive);
        return node;
    case ValidationShape::Kind::BooleanLiteral:
        node.shape = ResultShape::BooleanLiteral;
        node.literal = ResultLiteral { .kind = ResultLiteral::Kind::Boolean, .boolean = shape.as<ValidationShape::BooleanLiteral>().value };
        return node;
    case ValidationShape::Kind::NumberLiteral:
        node.shape = ResultShape::NumberLiteral;
        node.literal = ResultLiteral { .kind = ResultLiteral::Kind::Number, .number = shape.as<ValidationShape::NumberLiteral>().value };
        return node;
    case ValidationShape::Kind::StringLiteral:
        node.shape = ResultShape::StringLiteral;
        node.literal = ResultLiteral { .kind = ResultLiteral::Kind::String, .string = shape.as<ValidationShape::StringLiteral>().value };
        return node;
    case ValidationShape::Kind::Array:
        node.shape = ResultShape::Array;
        node.children.append(ResultNodeRef::forValidation(expectedResult, shape.as<ValidationShape::Array>().element));
        return node;
    case ValidationShape::Kind::Tuple:
        node.shape = ResultShape::Tuple;
        describeChildren(expectedResult, shape.as<ValidationShape::Tuple>().elements, node);
        return node;
    case ValidationShape::Kind::Dictionary:
        node.shape = ResultShape::Dictionary;
        node.children.append(ResultNodeRef::forValidation(expectedResult, shape.as<ValidationShape::Dictionary>().value));
        return node;
    case ValidationShape::Kind::Object:
        node.shape = ResultShape::Object;
        describeObject(nodes, expectedResult, shape.as<ValidationShape::Object>(), node);
        return node;
    case ValidationShape::Kind::Union:
        node.shape = ResultShape::Union;
        describeChildren(expectedResult, shape.as<ValidationShape::Union>().arms, node);
        return node;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ResultNode describeNode(const Program& program, ResultNodeRef ref)
{
    switch (ref.source) {
    case ResultNodeSource::Type:
        return describeTypeNode(program, ref.type(), ref.part);
    case ResultNodeSource::Validation:
        return describeValidationNode(program, ref.expectedResult(), ref.part);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Value makeArrayOf(const Program& program, const ResultNode& node, std::span<const ValueID> elements)
{
    const ClassPart* part = program.at(node.type).findClassPart(BuiltinClasses::Array);
    Value array = IRBuilder::make(Opcode::NewArray);
    array.setContents(part ? part->contentsAt(program.builtins(), ContentsSlot::Element) : TypeTable::anything);
    array.setElementsOperands(elements);
    return array;
}

static bool isLiteral(ResultShape shape)
{
    switch (shape) {
    case ResultShape::Null:
    case ResultShape::BooleanLiteral:
    case ResultShape::NumberLiteral:
    case ResultShape::StringLiteral:
        return true;
    case ResultShape::Unknown:
    case ResultShape::Never:
    case ResultShape::Undefined:
    case ResultShape::AnyObject:
    case ResultShape::String:
    case ResultShape::Number:
    case ResultShape::Boolean:
    case ResultShape::BigInt:
    case ResultShape::Array:
    case ResultShape::Tuple:
    case ResultShape::Dictionary:
    case ResultShape::Object:
    case ResultShape::Union:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<ResultLiteral> findTag(const Program& program, const ResultNode& arm, PropertyKeyID key)
{
    if (arm.shape != ResultShape::Object)
        return std::nullopt;
    const ResultMember* member = std::ranges::find(arm.members, key, &ResultMember::key);
    if (member == arm.members.end())
        return std::nullopt;
    // Every field of a layout may be missing, and a result that is not validated stays plain data where no arm is chosen. A validator has to try the arm of a missing tag.
    if (member->isOptional && member->node.isValidated())
        return std::nullopt;

    ResultNode tag = describeNode(program, member->node);
    bool isTested = std::ranges::any_of(tag.rules, [](const ValidationRule& rule) {
        return subjectOf(rule) != RuleSubject::Nothing;
    });
    if (!isLiteral(tag.shape) || tag.defaultValue || isTested)
        return std::nullopt;
    return tag.literal;
}

static std::optional<TaggedArms> partitionArmsByTag(const Program& program, std::span<const ResultNodeRef> arms, std::span<const ResultNode> nodes, PropertyKeyID key)
{
    TaggedArms result;
    result.discriminant.key = key;
    for (size_t index = 0; index < arms.size(); ++index) {
        std::optional<ResultLiteral> tag = findTag(program, nodes[index], key);
        if (!tag) {
            result.untagged.append(arms[index]);
            continue;
        }
        if (result.discriminant.tagOfArm.contains(*tag))
            return std::nullopt;
        result.discriminant.tagOfArm.append(*tag);
        result.tagged.append(arms[index]);
    }
    if (result.tagged.size() < 2)
        return std::nullopt;
    return result;
}

std::optional<TaggedArms> findTaggedArms(const Program& program, std::span<const ResultNodeRef> arms)
{
    Vector<ResultNode, 4> nodes(arms.size(), [&](size_t index) {
        return describeNode(program, arms[index]);
    });
    size_t numberOfObjects = std::ranges::count(nodes, ResultShape::Object, &ResultNode::shape);

    std::optional<TaggedArms> firstPartition;
    Vector<PropertyKeyID, 8> triedKeys;
    for (const ResultNode& node : nodes) {
        for (const ResultMember& member : node.members) {
            if (triedKeys.contains(member.key) || !findTag(program, node, member.key))
                continue;
            triedKeys.append(member.key);
            std::optional<TaggedArms> partition = partitionArmsByTag(program, arms, nodes.span(), member.key);
            if (!partition)
                continue;
            if (partition->tagged.size() == numberOfObjects)
                return partition;
            if (!firstPartition)
                firstPartition = WTF::move(partition);
        }
    }
    return firstPartition;
}

BitVector nodesReachableFrom(const Program& program, ExpectedResultID expectedResult, uint32_t node)
{
    const Vector<ValidationNode>& nodes = program.at(expectedResult).validation;
    BitVector reachable(nodes.size());
    reachable.set(node);
    Vector<uint32_t, 16> worklist { node };
    while (!worklist.isEmpty()) {
        forEachChild(nodes[worklist.takeLast()].shape, [&](uint32_t child) {
            if (reachable.add(child))
                worklist.append(child);
        });
    }
    return reachable;
}

ExpectedResultID canonicalValidationOf(const Program& program, ExpectedResultID expectedResult)
{
    const Vector<ValidationNode>& validation = program.at(expectedResult).validation;
    for (uint32_t index = 0; index < expectedResult.index(); ++index) {
        if (program.expectedResults[index].validation == validation)
            return ExpectedResultID(index);
    }
    return expectedResult;
}

DerivedFunctionKey parserKeyOf(ResultNodeRef ref)
{
    switch (ref.source) {
    case ResultNodeSource::Type:
        RELEASE_ASSERT(!ref.part);
        return derivedFunctionKey(FunctionRole::Parser, ref.type());
    case ResultNodeSource::Validation:
        return derivedFunctionKey(FunctionRole::ValidatingParser, ref.expectedResult(), ref.part);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

DerivedFunctionKey validatorKeyOf(ResultNodeRef ref)
{
    RELEASE_ASSERT(ref.isValidated());
    return derivedFunctionKey(FunctionRole::Validator, ref.expectedResult(), ref.part);
}

ResultNodeRef nodeOfKey(const DerivedFunctionKey& key)
{
    if (key.role == FunctionRole::Parser)
        return ResultNodeRef::forType(TypeID(key.parameter));
    RELEASE_ASSERT(key.role == FunctionRole::ValidatingParser || key.role == FunctionRole::Validator);
    return ResultNodeRef::forValidation(ExpectedResultID(key.parameter), key.node);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
