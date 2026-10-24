/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSelectTypedResultsNodes.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFlowResults.h"
#include "AOTVector.h"
#include <algorithm>
#include <utility>
#include <wtf/HashSet.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OptionSet<JSONValueClass> allValueClasses {
    JSONValueClass::Null,
    JSONValueClass::Boolean,
    JSONValueClass::Number,
    JSONValueClass::String,
    JSONValueClass::Array,
    JSONValueClass::Object,
};

static constexpr OptionSet<JSONValueClass> containerClasses { JSONValueClass::Array, JSONValueClass::Object };

static OptionSet<JSONValueClass> valueClassesOfShape(ResultShape shape)
{
    switch (shape) {
    case ResultShape::Unknown:
        return allValueClasses;
    case ResultShape::Never:
    case ResultShape::Undefined:
    case ResultShape::BigInt:
    case ResultShape::Union:
        return { };
    case ResultShape::Null:
        return JSONValueClass::Null;
    case ResultShape::Boolean:
    case ResultShape::BooleanLiteral:
        return JSONValueClass::Boolean;
    case ResultShape::Number:
    case ResultShape::NumberLiteral:
        return JSONValueClass::Number;
    case ResultShape::String:
    case ResultShape::StringLiteral:
        return JSONValueClass::String;
    case ResultShape::AnyObject:
        return containerClasses;
    case ResultShape::Array:
    case ResultShape::Tuple:
        return JSONValueClass::Array;
    case ResultShape::Dictionary:
    case ResultShape::Object:
        return JSONValueClass::Object;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

OptionSet<JSONValueClass> valueClassesOf(const Program& program, ResultNodeRef ref)
{
    OptionSet<JSONValueClass> classes;
    forEachAlternative(program, ref, [&](const ResultNode& node) {
        classes.add(valueClassesOfShape(node.shape));
    });
    return classes;
}

std::optional<ResultDiscriminant> findDiscriminant(const Program& program, std::span<const ResultNodeRef> objectArms)
{
    std::optional<TaggedArms> arms = findTaggedArms(program, objectArms);
    if (!arms || !arms->untagged.isEmpty())
        return std::nullopt;
    return WTF::move(arms->discriminant);
}

static void forEachChild(const ResultNode& node, NOESCAPE const Invocable<void(ResultNodeRef)> auto& visit)
{
    for (ResultNodeRef child : node.children)
        visit(child);
    for (const ResultMember& member : node.members)
        visit(member.node);
    if (node.undeclaredValue)
        visit(*node.undeclaredValue);
}

static bool canReachThroughTypes(const Program& program, ResultNodeRef start, ResultNodeRef target)
{
    HashSet<std::pair<TypeID, uint32_t>> visited;
    visited.add({ start.type(), start.part });
    Vector<ResultNodeRef, 16> worklist { start };
    bool isReached = false;
    while (!isReached && !worklist.isEmpty()) {
        forEachChild(describeNode(program, worklist.takeLast()), [&](ResultNodeRef child) {
            if (child == target)
                isReached = true;
            else if (visited.add({ child.type(), child.part }).isNewEntry)
                worklist.append(child);
        });
    }
    return isReached;
}

static bool canReach(const Program& program, ResultNodeRef start, ResultNodeRef target)
{
    ASSERT(start.source == target.source);
    switch (start.source) {
    case ResultNodeSource::Type:
        return canReachThroughTypes(program, start, target);
    case ResultNodeSource::Validation:
        return nodesReachableFrom(program, start.expectedResult(), start.part).get(target.part);
    }
    RELEASE_ASSERT_NOT_REACHED();
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

    for (JSONValueClass valueClass : containerClasses) {
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

bool allocatesLayout(const NodeDecisionInputs& inputs, const ResultNode& node)
{
    return allocatesLayout(inputs, node.layout);
}

static bool hasOnlyTextKeys(const Program& program, const ResultNode& object)
{
    return std::ranges::all_of(object.members, [&](const ResultMember& member) {
        return program.at(member.key).kind == PropertyKeyKind::Text;
    });
}

NodeDecision decideNodeCode(const NodeDecisionInputs& inputs, ResultNodeRef ref, NodeUse use)
{
    RELEASE_ASSERT(use == NodeUse::ParsedFromText || ref.isValidated());
    if (ref.isValidated() && ref.part >= inputs.build.limit(LimitID::MaxParserPlanNodes))
        return { NodeCode::Interpreted, InterpretedCause::TooManyNodes };

    ResultNode node = describeNode(inputs.program, ref);
    switch (node.shape) {
    case ResultShape::Union:
        if (hasRepeatedRecursion(inputs.program, ref, node))
            return { NodeCode::Interpreted, InterpretedCause::RepeatedRecursion };
        break;
    case ResultShape::Object:
        if (!hasOnlyTextKeys(inputs.program, node))
            return { NodeCode::Interpreted, InterpretedCause::MemberKeyIsNotText };
        if (!ref.isValidated() && !allocatesLayout(inputs, node))
            return { NodeCode::Interpreted, InterpretedCause::LayoutIsNotModeled };
        break;
    case ResultShape::Unknown:
    case ResultShape::Never:
    case ResultShape::Undefined:
    case ResultShape::Null:
    case ResultShape::AnyObject:
    case ResultShape::String:
    case ResultShape::Number:
    case ResultShape::Boolean:
    case ResultShape::BigInt:
    case ResultShape::BooleanLiteral:
    case ResultShape::NumberLiteral:
    case ResultShape::StringLiteral:
    case ResultShape::Array:
    case ResultShape::Tuple:
    case ResultShape::Dictionary:
        break;
    }
    return { NodeCode::Compiled, InterpretedCause::None };
}

static bool createsEveryArrayItself(const NodeDecisionInputs& inputs, ResultNodeRef ref, NodeUse use, BitVector& visitedParts)
{
    if (decideNodeCode(inputs, ref, use).code == NodeCode::Interpreted)
        return false;

    ResultNode node = describeNode(inputs.program, ref);
    switch (node.shape) {
    case ResultShape::Unknown:
    case ResultShape::AnyObject:
        return false;
    case ResultShape::Union:
        return std::ranges::all_of(node.children, [&](ResultNodeRef arm) {
            return !visitedParts.add(arm.part) || createsEveryArrayItself(inputs, arm, use, visitedParts);
        });
    case ResultShape::Never:
    case ResultShape::Undefined:
    case ResultShape::Null:
    case ResultShape::String:
    case ResultShape::Number:
    case ResultShape::Boolean:
    case ResultShape::BigInt:
    case ResultShape::BooleanLiteral:
    case ResultShape::NumberLiteral:
    case ResultShape::StringLiteral:
    case ResultShape::Array:
    case ResultShape::Tuple:
    case ResultShape::Dictionary:
    case ResultShape::Object:
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool createsEveryArrayItself(const NodeDecisionInputs& inputs, ResultNodeRef ref, NodeUse use)
{
    BitVector visitedParts;
    visitedParts.set(ref.part);
    return createsEveryArrayItself(inputs, ref, use, visitedParts);
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

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
