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
#include <wtf/BitVector.h>
#include <wtf/OptionSet.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class FlowResults;

enum class ResultNodeSource : uint8_t { Type, Validation };

struct ResultNodeRef {
    static ResultNodeRef forType(TypeID type, uint32_t part = 0) { return { ResultNodeSource::Type, type.index(), part }; }
    static ResultNodeRef forValidation(ExpectedResultID expectedResult, uint32_t node) { return { ResultNodeSource::Validation, expectedResult.index(), node }; }

    bool isValidated() const { return source == ResultNodeSource::Validation; }
    TypeID type() const
    {
        RELEASE_ASSERT(!isValidated());
        return TypeID(id);
    }
    ExpectedResultID expectedResult() const
    {
        RELEASE_ASSERT(isValidated());
        return ExpectedResultID(id);
    }

    friend bool operator==(const ResultNodeRef&, const ResultNodeRef&) = default;

    ResultNodeSource source { ResultNodeSource::Type };
    uint32_t id { 0 };
    uint32_t part { 0 };
};

enum class ResultShape : uint8_t {
    Unknown,
    Never,
    Undefined,
    Null,
    AnyObject,
    String,
    Number,
    Boolean,
    BigInt,
    BooleanLiteral,
    NumberLiteral,
    StringLiteral,
    Array,
    Tuple,
    Dictionary,
    Object,
    Union,
};

using ResultShapeSet = EnumeratorBitSet<ResultShape, static_cast<unsigned>(ResultShape::Union) + 1>;

struct ResultLiteral {
    enum class Kind : uint8_t { Null, Boolean, Number, String };

    friend bool operator==(const ResultLiteral&, const ResultLiteral&) = default;

    Kind kind { Kind::Null };
    bool boolean { false };
    double number { 0 };
    StringID string;
};

struct ResultMember {
    PropertyKeyID key;
    ResultNodeRef node;
    std::optional<uint32_t> field;
    bool isOptional { false };
    bool mustBeOwn { false };
};

struct ResultNode {
    bool isContainer() const { return shape >= ResultShape::Array; }

    void forEachChild(NOESCAPE const Invocable<void(ResultNodeRef)> auto& visit) const
    {
        for (ResultNodeRef child : children)
            visit(child);
        for (const ResultMember& member : members)
            visit(member.node);
        if (undeclaredValue)
            visit(*undeclaredValue);
    }

    ResultShape shape { ResultShape::Unknown };
    TypeID type;
    ResultLiteral literal;
    LayoutID layout;
    Vector<ResultMember, 8> members;
    UndeclaredMembers undeclared { UndeclaredMembers::Allowed };
    std::optional<ResultNodeRef> undeclaredValue;
    Vector<ResultNodeRef, 4> children;
    std::span<const ValidationRule> rules;
    std::optional<ResultLiteral> defaultValue;
    StringID message;
    StringID text;
};

ResultNode describeNode(const Program&, ResultNodeRef);
Value makeArrayOf(const Program&, const ResultNode& arrayOrTuple, std::span<const ValueID> elements);

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
            RELEASE_ASSERT(arm.source == root.source && arm.id == root.id);
            if (visitedParts.add(arm.part))
                worklist.append(arm);
        }
    }
}

enum class RuntimeClass : uint8_t {
    Undefined = 1 << 0,
    Null = 1 << 1,
    Boolean = 1 << 2,
    Number = 1 << 3,
    String = 1 << 4,
    BigInt = 1 << 5,
    Array = 1 << 6,
    Object = 1 << 7,
};

// What JSON text can spell. Each has the bit of the runtime class of its name.
enum class JSONValueClass : uint8_t {
    Null = 1 << 1,
    Boolean = 1 << 2,
    Number = 1 << 3,
    String = 1 << 4,
    Array = 1 << 6,
    Object = 1 << 7,
};

OptionSet<RuntimeClass> runtimeClassesOf(const Program&, ResultNodeRef);
OptionSet<JSONValueClass> valueClassesOf(const Program&, ResultNodeRef);
bool appliesDefault(const Program&, ResultNodeRef);

struct ResultDiscriminant {
    PropertyKeyID key;
    Vector<ResultLiteral, 4> tagOfArm;
};

struct TaggedArms {
    ResultDiscriminant discriminant;
    Vector<ResultNodeRef, 4> tagged;
    Vector<ResultNodeRef, 4> untagged;
};

std::optional<ResultDiscriminant> findDiscriminant(const Program&, std::span<const ResultNodeRef> objectArms);
std::optional<TaggedArms> findTaggedArms(const Program&, std::span<const ResultNodeRef> arms);

enum class NodeCode : uint8_t { Interpreted, Compiled };

struct NodeDecision {
    NodeCode code { NodeCode::Interpreted };
    InterpretedCause cause { InterpretedCause::None };
};

enum class NodeUse : uint8_t { ParsedFromText, CopiedFromValue };

struct NodeDecisionInputs {
    const Program& program;
    const FlowResults& flow;
    BuildContext& build;
};

NodeDecision decideNodeCode(const NodeDecisionInputs&, ResultNodeRef, NodeUse);
// No alternative of the node hands on an array of the caller, of the generic parser or of the interpreter.
bool createsEveryArrayItself(const NodeDecisionInputs&, ResultNodeRef, NodeUse);
LayoutID firstLayoutOf(const Program&, ResultNodeRef);
void addInterpretedNodeDecline(const NodeDecisionInputs&, LayoutID declaration, InterpretedCause);
bool allocatesLayout(const NodeDecisionInputs&, LayoutID);
inline bool allocatesLayout(const NodeDecisionInputs& inputs, const ResultNode& node) { return allocatesLayout(inputs, node.layout); }
bool canRepeatRecursion(const Program&, ResultNodeRef unionNode);
BitVector nodesReachableFrom(const Program&, ExpectedResultID, uint32_t node);

ExpectedResultID canonicalValidationOf(const Program&, ExpectedResultID);

DerivedFunctionKey parserKeyOf(ResultNodeRef);
DerivedFunctionKey validatorKeyOf(ResultNodeRef);
ResultNodeRef nodeOfKey(const DerivedFunctionKey&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
