/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTDerivedFunctions.h"
#include "AOTIR.h"
#include "AOTVector.h"
#include <optional>
#include <span>
#include "AOTBitVector.h"
#include <wtf/OptionSet.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class FlowResults;

// Part 0 is the whole type. Part n is its n-th alternative.
struct ResultNodeRef {
    friend bool operator==(const ResultNodeRef&, const ResultNodeRef&) = default;

    TypeID type;
    uint32_t part { 0 };
};

enum class ResultShape : uint8_t {
    Unknown,
    Null,
    String,
    Number,
    Boolean,
    BooleanLiteral,
    StringLiteral,
    Array,
    Object,
    Union,
};

using ResultShapeSet = EnumeratorBitSet<ResultShape, static_cast<unsigned>(ResultShape::Union) + 1>;

struct ResultLiteral {
    enum class Kind : uint8_t { Null, Boolean, String };

    friend bool operator==(const ResultLiteral&, const ResultLiteral&) = default;

    Kind kind { Kind::Null };
    bool boolean { false };
    StringID string;
};

struct ResultMember {
    PropertyKeyID key;
    ResultNodeRef node;
    uint32_t field { 0 };
    bool isOptional { false };
};

struct ResultNode {
    bool isContainer() const { return shape >= ResultShape::Array; }

    void forEachChild(NOESCAPE const Invocable<void(ResultNodeRef)> auto& visit) const
    {
        for (ResultNodeRef child : children)
            visit(child);
        for (const ResultMember& member : members)
            visit(member.node);
    }

    ResultShape shape { ResultShape::Unknown };
    TypeID type;
    ResultLiteral literal;
    LayoutID layout;
    Vector<ResultMember, 8> members;
    Vector<ResultNodeRef, 4> children;
};

ResultNode describeNode(const Program&, ResultNodeRef);

Value makeArrayOf(const Program&, const ResultNode& array);

// Visits the node, and the arms of every union among what it visits.
void forEachAlternative(const Program& program, ResultNodeRef root, NOESCAPE const Invocable<void(const ResultNode&)> auto& functor)
{
    BitVector visitedParts;
    visitedParts.set(root.part);
    Vector<ResultNodeRef, 8> worklist { root };
    while (!worklist.isEmpty()) {
        ResultNode node = describeNode(program, worklist.takeLast());
        functor(node);
        if (node.shape != ResultShape::Union)
            continue;
        for (ResultNodeRef arm : node.children) {
            RELEASE_ASSERT(arm.type == root.type);
            if (visitedParts.add(arm.part))
                worklist.append(arm);
        }
    }
}

// What JSON text can spell.
enum class JSONValueClass : uint8_t {
    Null = 1 << 0,
    Boolean = 1 << 1,
    Number = 1 << 2,
    String = 1 << 3,
    Array = 1 << 4,
    Object = 1 << 5,
};

OptionSet<JSONValueClass> valueClassesOf(const Program&, ResultNodeRef);

struct ResultDiscriminant {
    PropertyKeyID key;
    Vector<ResultLiteral, 4> tagOfArm;
};

struct TaggedArms {
    ResultDiscriminant discriminant;
    Vector<ResultNodeRef, 4> tagged;
    Vector<ResultNodeRef, 4> untagged;
};

std::optional<TaggedArms> findTaggedArms(const Program&, std::span<const ResultNodeRef> arms);

enum class NodeCode : uint8_t { Interpreted, Compiled };

struct NodeDecision {
    NodeCode code { NodeCode::Interpreted };
    InterpretedCause cause { InterpretedCause::None };
};

struct NodeDecisionInputs {
    const Program& program;
    const FlowResults& flow;
    BuildContext& build;
};

NodeDecision decideNodeCode(const NodeDecisionInputs&, ResultNodeRef);
LayoutID firstLayoutOf(const Program&, ResultNodeRef);
void addInterpretedNodeDecline(const NodeDecisionInputs&, LayoutID declaration, InterpretedCause);
bool allocatesLayout(const NodeDecisionInputs&, LayoutID);
inline bool allocatesLayout(const NodeDecisionInputs& inputs, const ResultNode& node) { return allocatesLayout(inputs, node.layout); }

DerivedFunctionKey parserKeyOf(ResultNodeRef);
ResultNodeRef nodeOfKey(const DerivedFunctionKey&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
