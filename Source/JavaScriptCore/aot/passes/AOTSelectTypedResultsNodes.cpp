/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSelectTypedResultsNodes.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinCalls.h"
#include "AOTExpandOwnKeysOrders.h"
#include "AOTFlowResults.h"
#include "AOTIRBuilder.h"
#include "AOTSelectTypedResultsTargets.h"
#include "AOTSelectTypedResultsTypeNodes.h"
#include "AOTSort.h"
#include "AOTUseLists.h"
#include "AOTValidationRules.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <functional>
#include <wtf/HashSet.h>

namespace JSC { namespace AOT { namespace Typed {

// The nodes of a type.

namespace {

struct FamilyOfMembers {
    LayoutSetID family;
    LayoutID owner;
    bool hasSeveralOwners { false };
};

} // anonymous namespace

static Vector<FamilyOfMembers> familiesOf(const Program& program, std::span<const LayoutID> members)
{
    Vector<FamilyOfMembers> families(members.size(), [&](size_t index) {
        return FamilyOfMembers { program.declaredMembersOf(members[index]), members[index] };
    });
    AOT::sort(families, { }, [](const FamilyOfMembers& family) {
        return std::pair { family.family, family.owner };
    });
    size_t numberOfFamilies = 0;
    for (const FamilyOfMembers& family : families) {
        if (numberOfFamilies && families[numberOfFamilies - 1].family == family.family)
            families[numberOfFamilies - 1].hasSeveralOwners = true;
        else
            families[numberOfFamilies++] = family;
    }
    families.shrink(numberOfFamilies);
    AOT::sort(families, std::ranges::greater { }, [&](const FamilyOfMembers& family) {
        return std::pair { program.at(family.family).members.size(), family.family };
    });
    return families;
}

static Vector<LayoutID, 4> headLayoutsOf(const Program& program, const Type& type)
{
    std::span<const LayoutID> members = membersOf(program, type);
    Vector<LayoutID, 4> candidates;
    for (LayoutID member : members) {
        if (program.at(member).kind == LayoutKind::Record)
            candidates.append(member);
    }
    if (candidates.isEmpty())
        return { };

    // A candidate that is removed by itself keeps its place in the list, which stays sorted, until the list is compacted.
    BitVector isRemoved;
    size_t numberLeft = candidates.size();
    auto compact = [&] {
        if (numberLeft == candidates.size())
            return;
        size_t index = 0;
        candidates.removeAllMatching([&](LayoutID) {
            return isRemoved.get(index++);
        });
        isRemoved.clearAll();
    };

    for (const FamilyOfMembers& family : familiesOf(program, members)) {
        if (!numberLeft)
            break;
        std::span<const LayoutID> subtypes = program.at(family.family).members.span();
        auto isOwnerAlone = [&](LayoutID layout) {
            return !family.hasSeveralOwners && layout == family.owner;
        };
        if (numberLeft <= subtypes.size()) {
            compact();
            candidates.removeAllMatching([&](LayoutID candidate) {
                return !isOwnerAlone(candidate) && std::ranges::binary_search(subtypes, candidate);
            });
            numberLeft = candidates.size();
            continue;
        }
        for (LayoutID subtype : subtypes) {
            if (isOwnerAlone(subtype))
                continue;
            auto position = std::ranges::lower_bound(candidates, subtype);
            if (position != candidates.end() && *position == subtype && !isRemoved.set(static_cast<size_t>(position - candidates.begin())))
                --numberLeft;
        }
    }
    compact();
    return candidates;
}

Vector<TypeAlternative, 4> alternativesOf(const Program& program, TypeID id)
{
    const Type& type = program.at(id);
    Vector<TypeAlternative, 4> alternatives;
    if (type.primitives & Primitive::Null)
        alternatives.append(TypeAlternative { .shape = ResultShape::Null });
    if (uint16_t booleans = type.primitives & Primitive::Boolean; booleans == Primitive::Boolean)
        alternatives.append(TypeAlternative { .shape = ResultShape::Boolean });
    else if (booleans)
        alternatives.append(TypeAlternative { .shape = ResultShape::BooleanLiteral, .literal = { .kind = ResultLiteral::Kind::Boolean, .boolean = booleans == Primitive::True } });
    if (type.primitives & Primitive::Number)
        alternatives.append(TypeAlternative { .shape = ResultShape::Number });
    if (type.primitives & Primitive::String)
        alternatives.append(TypeAlternative { .shape = ResultShape::String });
    else {
        for (StringID string : type.stringLiterals)
            alternatives.append(TypeAlternative { .shape = ResultShape::StringLiteral, .literal = { .kind = ResultLiteral::Kind::String, .string = string } });
    }
    if (const ClassPart* array = type.findClassPart(BuiltinClasses::Array))
        alternatives.append(TypeAlternative { .shape = ResultShape::Array, .contents = array->contentsAt(program.builtins(), ContentsSlot::Element) });
    for (LayoutID layout : headLayoutsOf(program, type))
        alternatives.append(TypeAlternative { .shape = ResultShape::Object, .layout = layout });
    return alternatives;
}

// What flow has proven about the arrays in a field says nothing about what a parser has to make. Fields that differ in that alone share a parser: the one of the type as it was read.
static TypeID withoutProofsAboutClasses(const Program& program, TypeID id)
{
    if (!id || TypeTable::isProvisional(id))
        return id;
    auto hasProof = [](const ClassPart& part) {
        return part.contentsAreProven || !part.provenPristine.isEmpty() || part.arrayStorage != ArrayStorage::Other;
    };
    if (std::ranges::none_of(program.at(id).classes, hasProof))
        return id;

    Type type = program.at(id);
    for (ClassPart& part : type.classes) {
        part.contentsAreProven = false;
        part.provenPristine = { };
        part.arrayStorage = ArrayStorage::Other;
    }
    type.canonicalize();
    return program.types.find(type).value_or(id);
}

static ResultNode describeAlternative(const Program& program, TypeID type, const TypeAlternative& alternative)
{
    static constexpr ResultShapeSet shapesOfAlternatives { ResultShape::Null, ResultShape::String, ResultShape::Number, ResultShape::Boolean, ResultShape::BooleanLiteral, ResultShape::StringLiteral, ResultShape::Array, ResultShape::Object };
    RELEASE_ASSERT(shapesOfAlternatives.contains(alternative.shape));

    ResultNode node { .shape = alternative.shape, .type = type, .literal = alternative.literal, .layout = alternative.layout };
    if (node.shape == ResultShape::Array)
        node.children.append(ResultNodeRef::forType(alternative.contents));
    if (node.shape != ResultShape::Object)
        return node;

    const Vector<Field>& fields = program.at(node.layout).fields;
    for (uint32_t index = 0; index < fields.size(); ++index) {
        const Field& field = fields[index];
        if (!isEnumerableStringField(program, field))
            continue;
        TypeID expectation = withoutProofsAboutClasses(program, expectationOf(field.declared));
        node.members.append(ResultMember {
            .key = field.key,
            .node = ResultNodeRef::forType(expectation ? expectation : TypeTable::anything),
            .field = index,
            .isOptional = !field.declared.type || program.at(field.declared.type).primitives & Primitive::Undefined,
        });
    }
    return node;
}

static ResultNode describeTypeNode(const Program& program, TypeID type, uint32_t part)
{
    Vector<TypeAlternative, 4> alternatives = alternativesOf(program, type);
    if (part) {
        RELEASE_ASSERT(part <= alternatives.size());
        return describeAlternative(program, type, alternatives[part - 1]);
    }

    ResultNode node { .type = type };
    if (program.at(type).anyObject || alternatives.isEmpty())
        return node;
    if (alternatives.size() == 1)
        return describeAlternative(program, type, alternatives[0]);

    node.shape = ResultShape::Union;
    for (uint32_t index = 0; index < alternatives.size(); ++index)
        node.children.append(ResultNodeRef::forType(type, index + 1));
    return node;
}

// The nodes of a validation.

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
        if (shape.is<ValidationShape::Unknown>() || shape.is<ValidationShape::Undefined>())
            return true;
        const auto* unionOfArms = shape.tryAs<ValidationShape::Union>();
        if (!unionOfArms)
            continue;
        for (uint32_t arm : unionOfArms->arms) {
            if (arm != root && visited.add(arm) && !hasDefault(nodes[arm]))
                alternatives.append(arm);
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

static ResultNode describeValidationNode(const Program& program, ExpectedResultID expectedResult, uint32_t part)
{
    using Shape = ValidationShape;
    const Vector<ValidationNode>& nodes = program.at(expectedResult).validation;
    const ValidationNode& source = nodes[part];
    ResultNode node { .type = source.type, .rules = source.rules.span(), .text = source.text };
    for (const ValidationRule& rule : node.rules) {
        if (!node.defaultValue)
            node.defaultValue = defaultOf(rule);
        if (const auto* message = rule.tryAs<ValidationRule::Message>(); message && !node.message)
            node.message = message->text;
    }

    auto refTo = [&](uint32_t child) {
        return ResultNodeRef::forValidation(expectedResult, child);
    };
    auto literal = [&](ResultShape shape, ResultLiteral value) {
        node.literal = value;
        return shape;
    };
    auto container = [&](ResultShape shape, std::span<const uint32_t> children) {
        node.children.appendContainerWithMapping(children, refTo);
        return shape;
    };
    node.shape = WTF::switchOn(source.shape.cases,
        [](const Shape::Unknown&) { return ResultShape::Unknown; },
        [](const Shape::Never&) { return ResultShape::Never; },
        [](const Shape::Undefined&) { return ResultShape::Undefined; },
        [](const Shape::Null&) { return ResultShape::Null; },
        [](const Shape::AnyObject&) { return ResultShape::AnyObject; },
        [](const Shape::Primitive& shape) { return shapeOf(shape.primitive); },
        [&](const Shape::BooleanLiteral& shape) { return literal(ResultShape::BooleanLiteral, { .kind = ResultLiteral::Kind::Boolean, .boolean = shape.value }); },
        [&](const Shape::NumberLiteral& shape) { return literal(ResultShape::NumberLiteral, { .kind = ResultLiteral::Kind::Number, .number = shape.value }); },
        [&](const Shape::StringLiteral& shape) { return literal(ResultShape::StringLiteral, { .kind = ResultLiteral::Kind::String, .string = shape.value }); },
        [&](const Shape::Array& shape) { return container(ResultShape::Array, singleElementSpan(shape.element)); },
        [&](const Shape::Tuple& shape) { return container(ResultShape::Tuple, shape.elements.span()); },
        [&](const Shape::Dictionary& shape) { return container(ResultShape::Dictionary, singleElementSpan(shape.value)); },
        [&](const Shape::Union& shape) { return container(ResultShape::Union, shape.arms.span()); },
        [&](const Shape::Object& shape) {
            node.layout = shape.layout;
            node.undeclared = shape.undeclared;
            if (shape.undeclaredValue)
                node.undeclaredValue = refTo(*shape.undeclaredValue);
            node.members.appendContainerWithMapping(shape.members, [&](const ValidationMember& member) {
                return ResultMember { member.key, refTo(member.node), member.field, member.isOptional, !member.isOptional && acceptsUndefined(nodes, member.node) };
            });
            return ResultShape::Object;
        });
    return node;
}

ResultNode describeNode(const Program& program, ResultNodeRef ref)
{
    return ref.isValidated() ? describeValidationNode(program, ref.expectedResult(), ref.part) : describeTypeNode(program, ref.type(), ref.part);
}

BitVector nodesReachableFrom(const Program& program, ExpectedResultID expectedResult, uint32_t node)
{
    const Vector<ValidationNode>& nodes = program.at(expectedResult).validation;
    BitVector reachable(nodes.size());
    reachable.set(node);
    Vector<uint32_t, 16> worklist { node };
    auto visit = [&](uint32_t child) {
        if (reachable.add(child))
            worklist.append(child);
    };
    while (!worklist.isEmpty()) {
        const ValidationShape& shape = nodes[worklist.takeLast()].shape;
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
            continue;
        case ValidationShape::Kind::Array:
            visit(shape.as<ValidationShape::Array>().element);
            continue;
        case ValidationShape::Kind::Dictionary:
            visit(shape.as<ValidationShape::Dictionary>().value);
            continue;
        case ValidationShape::Kind::Tuple:
            std::ranges::for_each(shape.as<ValidationShape::Tuple>().elements, visit);
            continue;
        case ValidationShape::Kind::Union:
            std::ranges::for_each(shape.as<ValidationShape::Union>().arms, visit);
            continue;
        case ValidationShape::Kind::Object: {
            const auto& object = shape.as<ValidationShape::Object>();
            std::ranges::for_each(object.members, visit, &ValidationMember::node);
            if (object.undeclaredValue)
                visit(*object.undeclaredValue);
            continue;
        }
        }
        RELEASE_ASSERT_NOT_REACHED();
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

// What holds for a node of either kind.

Value makeArrayOf(const Program& program, const ResultNode& node, std::span<const ValueID> elements)
{
    const ClassPart* part = program.at(node.type).findClassPart(BuiltinClasses::Array);
    Value array = IRBuilder::make(Opcode::NewArray);
    array.setContents(part ? part->contentsAt(program.builtins(), ContentsSlot::Element) : TypeTable::anything);
    array.setElementsOperands(elements);
    return array;
}

static OptionSet<RuntimeClass> classesAcceptedBy(ResultShape shape)
{
    static constexpr std::pair<RuntimeClass, ResultShapeSet> shapesThatAccept[] {
        { RuntimeClass::Undefined, { ResultShape::Unknown, ResultShape::Undefined } },
        { RuntimeClass::Null, { ResultShape::Unknown, ResultShape::Null } },
        { RuntimeClass::Boolean, { ResultShape::Unknown, ResultShape::Boolean, ResultShape::BooleanLiteral } },
        { RuntimeClass::Number, { ResultShape::Unknown, ResultShape::Number, ResultShape::NumberLiteral } },
        { RuntimeClass::String, { ResultShape::Unknown, ResultShape::String, ResultShape::StringLiteral } },
        { RuntimeClass::BigInt, { ResultShape::Unknown, ResultShape::BigInt } },
        { RuntimeClass::Array, { ResultShape::Unknown, ResultShape::AnyObject, ResultShape::Array, ResultShape::Tuple } },
        { RuntimeClass::Object, { ResultShape::Unknown, ResultShape::AnyObject, ResultShape::Dictionary, ResultShape::Object } },
    };
    OptionSet<RuntimeClass> classes;
    for (const auto& [runtimeClass, shapes] : shapesThatAccept) {
        if (shapes.contains(shape))
            classes.add(runtimeClass);
    }
    return classes;
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

OptionSet<JSONValueClass> valueClassesOf(const Program& program, ResultNodeRef ref)
{
    return OptionSet<JSONValueClass>::fromRaw((runtimeClassesOf(program, ref) - OptionSet<RuntimeClass> { RuntimeClass::Undefined, RuntimeClass::BigInt }).toRaw());
}

bool appliesDefault(const Program& program, ResultNodeRef ref)
{
    bool hasDefault = false;
    forEachAlternative(program, ref, [&](const ResultNode& node) {
        hasDefault |= node.defaultValue.has_value();
    });
    return hasDefault;
}

LayoutID firstLayoutOf(const Program& program, ResultNodeRef ref)
{
    LayoutID first;
    forEachAlternative(program, ref, [&](const ResultNode& node) {
        if (node.layout && (!first || node.layout < first))
            first = node.layout;
    });
    return first;
}

static std::optional<ResultLiteral> findTag(const Program& program, const ResultNode& arm, PropertyKeyID key)
{
    static constexpr ResultShapeSet literalShapes { ResultShape::Null, ResultShape::BooleanLiteral, ResultShape::NumberLiteral, ResultShape::StringLiteral };
    if (arm.shape != ResultShape::Object)
        return std::nullopt;
    const ResultMember* member = std::ranges::find(arm.members, key, &ResultMember::key);
    // Every field of a layout may be missing, and a result that is not validated stays plain data where no arm is chosen. A validator has to try the arm of a missing tag.
    if (member == arm.members.end() || (member->isOptional && member->node.isValidated()))
        return std::nullopt;

    ResultNode tag = describeNode(program, member->node);
    bool isTested = std::ranges::any_of(tag.rules, [](const ValidationRule& rule) {
        return subjectOf(rule) != RuleSubject::Nothing;
    });
    if (!literalShapes.contains(tag.shape) || tag.defaultValue || isTested)
        return std::nullopt;
    return tag.literal;
}

static std::optional<TaggedArms> partitionArmsByTag(const Program& program, std::span<const ResultNodeRef> arms, std::span<const ResultNode> nodes, PropertyKeyID key)
{
    TaggedArms result { .discriminant = { .key = key } };
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

std::optional<ResultDiscriminant> findDiscriminant(const Program& program, std::span<const ResultNodeRef> objectArms)
{
    std::optional<TaggedArms> arms = findTaggedArms(program, objectArms);
    if (!arms || !arms->untagged.isEmpty())
        return std::nullopt;
    return WTF::move(arms->discriminant);
}

// Which nodes are compiled.

static bool canReach(const Program& program, ResultNodeRef start, ResultNodeRef target)
{
    ASSERT(start.source == target.source);
    if (start.isValidated())
        return nodesReachableFrom(program, start.expectedResult(), start.part).get(target.part);

    HashSet<std::pair<TypeID, uint32_t>> visited;
    visited.add({ start.type(), start.part });
    Vector<ResultNodeRef, 16> worklist { start };
    bool isReached = false;
    while (!isReached && !worklist.isEmpty()) {
        describeNode(program, worklist.takeLast()).forEachChild([&](ResultNodeRef child) {
            if (child == target)
                isReached = true;
            else if (visited.add({ child.type(), child.part }).isNewEntry)
                worklist.append(child);
        });
    }
    return isReached;
}

static bool triesSeveralRecursiveArms(const Program& program, std::span<const ResultNodeRef> candidates, ResultNodeRef unionRef)
{
    auto isRecursive = [&](ResultNodeRef arm) {
        return canReach(program, arm, unionRef);
    };

    std::optional<TaggedArms> arms = findTaggedArms(program, candidates);
    unsigned numberOfRecursiveTrials = arms && std::ranges::any_of(arms->tagged, isRecursive) ? 1 : 0;
    for (ResultNodeRef arm : arms ? arms->untagged.span() : candidates) {
        if (isRecursive(arm) && ++numberOfRecursiveTrials > 1)
            return true;
    }
    return false;
}

static bool hasRepeatedRecursion(const Program& program, ResultNodeRef unionRef, const ResultNode& unionNode)
{
    std::span<const ResultNodeRef> arms = unionNode.children.span();
    Vector<OptionSet<JSONValueClass>, 4> classesOfArm(arms.size(), [&](size_t index) {
        return valueClassesOf(program, arms[index]);
    });

    for (JSONValueClass valueClass : { JSONValueClass::Array, JSONValueClass::Object }) {
        Vector<ResultNodeRef, 4> candidates;
        for (size_t index = 0; index < arms.size(); ++index) {
            if (classesOfArm[index].contains(valueClass))
                candidates.append(arms[index]);
        }
        if (candidates.size() > 1 && triesSeveralRecursiveArms(program, candidates.span(), unionRef))
            return true;
    }
    return false;
}

bool canRepeatRecursion(const Program& program, ResultNodeRef unionNode)
{
    ResultNode node = describeNode(program, unionNode);
    RELEASE_ASSERT(node.shape == ResultShape::Union);
    return hasRepeatedRecursion(program, unionNode, node);
}

bool allocatesLayout(const NodeDecisionInputs& inputs, LayoutID layout)
{
    if (!layout || inputs.program.at(layout).flow.allocation == AllocationState::NeverAllocated)
        return false;
    return !inputs.flow.hasResults() || !!inputs.flow.typedResultSite(layout);
}

NodeDecision decideNodeCode(const NodeDecisionInputs& inputs, ResultNodeRef ref, NodeUse use)
{
    RELEASE_ASSERT(use == NodeUse::ParsedFromText || ref.isValidated());
    if (ref.isValidated() && ref.part >= inputs.build.limit(LimitID::MaxParserPlanNodes))
        return { NodeCode::Interpreted, InterpretedCause::TooManyNodes };

    ResultNode node = describeNode(inputs.program, ref);
    if (node.shape == ResultShape::Union && hasRepeatedRecursion(inputs.program, ref, node))
        return { NodeCode::Interpreted, InterpretedCause::RepeatedRecursion };
    if (node.shape == ResultShape::Object) {
        bool hasOnlyTextKeys = std::ranges::all_of(node.members, [&](const ResultMember& member) {
            return inputs.program.at(member.key).kind == PropertyKeyKind::Text;
        });
        if (!hasOnlyTextKeys)
            return { NodeCode::Interpreted, InterpretedCause::MemberKeyIsNotText };
        if (!ref.isValidated() && !allocatesLayout(inputs, node))
            return { NodeCode::Interpreted, InterpretedCause::LayoutIsNotModeled };
    }
    return { NodeCode::Compiled, InterpretedCause::None };
}

static bool createsEveryArrayItself(const NodeDecisionInputs& inputs, ResultNodeRef ref, NodeUse use, BitVector& visitedParts)
{
    if (decideNodeCode(inputs, ref, use).code == NodeCode::Interpreted)
        return false;

    ResultNode node = describeNode(inputs.program, ref);
    if (node.shape != ResultShape::Union)
        return node.shape != ResultShape::Unknown && node.shape != ResultShape::AnyObject;
    return std::ranges::all_of(node.children, [&](ResultNodeRef arm) {
        return !visitedParts.add(arm.part) || createsEveryArrayItself(inputs, arm, use, visitedParts);
    });
}

bool createsEveryArrayItself(const NodeDecisionInputs& inputs, ResultNodeRef ref, NodeUse use)
{
    BitVector visitedParts;
    visitedParts.set(ref.part);
    return createsEveryArrayItself(inputs, ref, use, visitedParts);
}

void addInterpretedNodeDecline(const NodeDecisionInputs& inputs, LayoutID declaration, InterpretedCause cause)
{
    RELEASE_ASSERT(cause != InterpretedCause::None);
    bool isReportedAsLimit = cause == InterpretedCause::TooManyNodes;
    if (isReportedAsLimit || !inputs.build.options().collectsDeclines())
        return;

    Subject subject { .layout = declaration };
    subject.location = findLocation(&inputs.program, subject).value_or(noSourceLocation);
    inputs.build.addDecline({
        .kind = DeclineKind::ResultNodeInterpreted,
        .limit = LimitID::None,
        .count = 0,
        .reason = encodeDeclineReason(DeclineKind::ResultNodeInterpreted, cause),
        .witness = witnessOf(subject),
    });
}

DerivedFunctionKey parserKeyOf(ResultNodeRef ref)
{
    if (ref.isValidated())
        return derivedFunctionKey(FunctionRole::ValidatingParser, ref.expectedResult(), ref.part);
    RELEASE_ASSERT(!ref.part);
    return derivedFunctionKey(FunctionRole::Parser, ref.type());
}

DerivedFunctionKey validatorKeyOf(ResultNodeRef ref)
{
    return derivedFunctionKey(FunctionRole::Validator, ref.expectedResult(), ref.part);
}

ResultNodeRef nodeOfKey(const DerivedFunctionKey& key)
{
    if (key.role == FunctionRole::Parser)
        return ResultNodeRef::forType(TypeID(key.parameter));
    RELEASE_ASSERT(key.role == FunctionRole::ValidatingParser || key.role == FunctionRole::Validator);
    return ResultNodeRef::forValidation(ExpectedResultID(key.parameter), key.node);
}

// The type that the users of a call ask of its result.

TypedResultForm typedResultFormOf(const Program& program, const Value& call)
{
    return call.opcode == Opcode::CallBuiltin ? program.builtins().at(call.overload()).result.typedResultForm : TypedResultForm::None;
}

ValueID typedResultSourceOf(const Program& program, const Value& call)
{
    if (call.opcode != Opcode::CallBuiltin)
        return { };
    BuiltinCall builtinCall = builtinCallOf(program, call);
    std::optional<BuiltinOperand> source = program.builtins().at(builtinCall.overload).result.typedResultSource;
    return source ? builtinCall.operandAt(*source) : ValueID();
}

std::optional<uint32_t> indexOfAwaitedOperand(const Value& value)
{
    if (value.opcode == Opcode::Await)
        return OperandIndex::Await::value;
    if (value.opcode == Opcode::AwaitBySavingFrame)
        return OperandIndex::AwaitBySavingFrame::awaited;
    return std::nullopt;
}

static Vector<TypeID, 4> collectRequestedTypes(const Program& program, const Function& function, const UseLists& uses, ValueID call)
{
    struct FollowedValue {
        ValueID value;
        bool mayBePromise;
    };

    Vector<TypeID, 4> requestedTypes;
    auto consider = [&](TypeID id) {
        if (!id || TypeTable::isProvisional(id))
            return;
        const Type& type = program.at(id);
        if (!membersOf(program, type).empty() || type.findClassPart(BuiltinClasses::Array))
            requestedTypes.append(id);
    };

    // Flow asks about calls that are not resolved to a built-in yet, and those record no form.
    TypedResultForm form = typedResultFormOf(program, function.at(call));
    Vector<FollowedValue, 4> worklist { { call, form != TypedResultForm::ParsedText && form != TypedResultForm::ValidatedValue } };
    BitVector visited;
    visited.set(call.index());

    while (!worklist.isEmpty()) {
        FollowedValue followed = worklist.takeLast();
        for (const Use& use : uses.usesOf(followed.value)) {
            if (use.kind == Use::Kind::TerminatorOperand && function.at(use.block).terminator.kind == TerminatorKind::ReturnCall) {
                for (TailCast cast : function.at(use.block).terminator.casts())
                    consider(cast.declared.type);
            }
            if (use.kind != Use::Kind::Operand)
                continue;

            const Value& user = function.at(use.user);
            // AddGenericVersion makes a test and a branch of a Check.
            if (user.opcode == Opcode::TypeTest && use.operandIndex == OperandIndex::TypeTest::value)
                consider(user.testedType());
            if (user.opcode == Opcode::CallDirect) {
                std::optional<uint32_t> entryParameter = user.variadicOperandIndex(use.operandIndex);
                const Function& callee = program.at(user.function());
                if (entryParameter && *entryParameter < callee.numberOfEntryParameters() && callee.entryParameterKind(*entryParameter) == EntryParameter::Parameter)
                    consider(callee.declaredEntryParameter(*entryParameter)->type);
            }

            bool isSameValue = isCast(user.opcode) && !use.operandIndex;
            bool isResolvedValue = followed.mayBePromise && indexOfAwaitedOperand(user) == use.operandIndex;
            if (!isSameValue && !isResolvedValue)
                continue;
            if (user.opcode == Opcode::Check)
                consider(user.castType());
            consider(user.expected());
            if (visited.add(use.user.index()))
                worklist.append({ use.user, followed.mayBePromise && isSameValue });
        }
    }
    return requestedTypes;
}

static bool isDeepSubset(const Program& program, TypeID narrow, TypeID wide)
{
    using TypePair = std::pair<TypeID, TypeID>;
    Vector<TypePair, 4> worklist { TypePair { narrow, wide } };
    HashSet<TypePair> visited;
    while (!worklist.isEmpty()) {
        TypePair pair = worklist.takeLast();
        if (pair.first == pair.second || !visited.add(pair).isNewEntry)
            continue;
        const Type& narrowType = program.at(pair.first);
        const Type& wideType = program.at(pair.second);
        if (!narrowType.isSubsetOf(wideType, program))
            return false;
        for (const ClassPart& part : narrowType.classes) {
            const ClassPart* partner = wideType.findClassPart(part.builtinClass);
            if (!partner)
                continue;
            for (unsigned index = 0; index < partner->contents.size(); ++index)
                worklist.append(TypePair { part.contentsAt(index), partner->contentsAt(index) });
        }
    }
    return true;
}

TypeID findTypeOfTypedResultTarget(const Program& program, const Function& function, const UseLists& uses, ValueID call)
{
    Vector<TypeID, 4> requestedTypes = collectRequestedTypes(program, function, uses, call);
    TypeID mostSpecific;
    for (TypeID requested : requestedTypes) {
        if (!mostSpecific || isDeepSubset(program, requested, mostSpecific))
            mostSpecific = requested;
    }
    for (TypeID requested : requestedTypes) {
        if (!isDeepSubset(program, mostSpecific, requested))
            return { };
    }
    return mostSpecific;
}

TypedResultTarget findTypedResultTarget(const Program& program, const Function& function, const UseLists& uses, ValueID call, size_t maxNumberOfLayouts, const HeadLayoutsOfFamilies* headLayouts)
{
    TypedResultTarget target { findTypeOfTypedResultTarget(program, function, uses, call), { } };
    if (!target)
        return { };
    auto append = [&](LayoutID layout) {
        target.layouts.append(layout);
        return target.layouts.size() > maxNumberOfLayouts ? IterationStatus::Done : IterationStatus::Continue;
    };
    forEachLayoutReachedBy(program, target.type, headLayouts, ScopedLambda<IterationStatus(LayoutID)>(append));
    AOT::sort(target.layouts);
    return target;
}

std::span<const LayoutID> HeadLayoutsOfFamilies::of(const Program& program, const Type& type) const
{
    {
        Locker locker { m_lock };
        if (auto found = m_headsOfFamily.find(type.layouts); found != m_headsOfFamily.end())
            return found->value->span();
    }
    auto heads = makeUniqueWithoutFastMallocCheck<Vector<LayoutID, 4>>(headLayoutsOf(program, type));
    Locker locker { m_lock };
    return m_headsOfFamily.ensure(type.layouts, [&] {
        return WTF::move(heads);
    }).iterator->value->span();
}

void forEachLayoutReachedBy(const Program& program, TypeID root, const HeadLayoutsOfFamilies* headLayouts, const ScopedLambda<IterationStatus(LayoutID)>& functor)
{
    HashSet<LayoutID> visitedLayouts;
    HashSet<LayoutSetID> visitedFamilies;
    HashSet<TypeID> visitedTypes;
    Vector<TypeID, 8> worklist;
    auto visit = [&](TypeID type) {
        if (type && visitedTypes.add(type).isNewEntry)
            worklist.append(type);
    };

    visit(root);
    while (!worklist.isEmpty()) {
        const Type& type = program.at(worklist.takeLast());
        if (const ClassPart* array = type.findClassPart(BuiltinClasses::Array))
            visit(array->contentsAt(program.builtins(), ContentsSlot::Element));
        if (!type.layouts || !visitedFamilies.add(type.layouts).isNewEntry)
            continue;
        Vector<LayoutID, 4> headsOfThisWalk;
        if (!headLayouts)
            headsOfThisWalk = headLayoutsOf(program, type);
        for (LayoutID layout : headLayouts ? headLayouts->of(program, type) : headsOfThisWalk.span()) {
            if (!visitedLayouts.add(layout).isNewEntry)
                continue;
            if (functor(layout) == IterationStatus::Done)
                return;
            for (const Field& field : program.at(layout).fields) {
                if (isEnumerableStringField(program, field))
                    visit(expectationOf(field.declared));
            }
        }
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
