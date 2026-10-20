/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTFlowResults.h"
#include "AOTFlowSiteData.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTResolveCallsDirectCall.h"
#include "AOTResultRules.h"
#include <optional>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct DataTreeSummary {
    uint64_t numberOfNodes { 0 };
    bool hasMemberWithoutField { false };
    bool hasNeverAllocatedLayout { false };
};

struct OpenNode {
    const DataNode* node { nullptr };
    uint32_t ordinal { 0 };
    size_t numberOfChildren { 0 };
    Vector<ValueID, 8> values;
    Vector<uint32_t, 2> holes;

    size_t numberOfFinishedChildren() const { return values.size() + holes.size(); }
    bool isComplete() const { return numberOfFinishedChildren() == numberOfChildren; }
};

struct DataExpansion {
    const Program& program;
    const FlowResults& flow;
    const ValueInsertion& insertion;
    AllocationSiteID rootSite;
};

} // anonymous namespace

static void addObjectTo(DataTreeSummary& summary, const Program& program, const DataNode::Object& object)
{
    const Layout& layout = program.at(object.layout);
    summary.hasNeverAllocatedLayout |= layout.flow.allocation == AllocationState::NeverAllocated;
    for (const DataMember& member : object.members)
        summary.hasMemberWithoutField |= !layout.findField(member.key);
}

static DataTreeSummary summarize(const Program& program, const DataNode& root)
{
    DataTreeSummary summary;
    forEachDataNodeInPreorder(root, [&](uint32_t, const DataNode& node) {
        ++summary.numberOfNodes;
        if (const auto* object = node.tryAs<DataNode::Object>())
            addObjectTo(summary, program, *object);
    });
    return summary;
}

// The read-only encoding of an object has fields only, so a tree with any other member is expanded whatever its size.
static bool shouldExpand(const DataTreeSummary& tree, uint64_t maxNodes)
{
    if (tree.hasNeverAllocatedLayout)
        return false;
    return tree.hasMemberWithoutField || tree.numberOfNodes <= maxNodes;
}

static std::optional<size_t> numberOfChildrenOf(const DataNode& node)
{
    switch (node.kind()) {
    case DataNode::Kind::Array:
        return node.as<DataNode::Array>().elements.size();
    case DataNode::Kind::Object:
        return node.as<DataNode::Object>().members.size();
    case DataNode::Kind::Undefined:
    case DataNode::Kind::Null:
    case DataNode::Kind::Hole:
    case DataNode::Kind::Boolean:
    case DataNode::Kind::Number:
    case DataNode::Kind::String:
    case DataNode::Kind::BigInt:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Value makeNumber(double number)
{
    if (isTrackedInteger(number)) {
        Value constant = IRBuilder::make(Opcode::Integer);
        constant.setInteger(static_cast<int64_t>(number));
        return constant;
    }
    Value constant = IRBuilder::make(Opcode::Double);
    constant.setNumber(number);
    return constant;
}

static Value makeConstant(const DataNode& leaf)
{
    switch (leaf.kind()) {
    case DataNode::Kind::Undefined:
        return IRBuilder::make(Opcode::Undefined);
    case DataNode::Kind::Null:
        return IRBuilder::make(Opcode::Null);
    case DataNode::Kind::Boolean: {
        Value constant = IRBuilder::make(Opcode::Boolean);
        constant.setBoolean(leaf.as<DataNode::Boolean>().value);
        return constant;
    }
    case DataNode::Kind::Number:
        return makeNumber(leaf.as<DataNode::Number>().value);
    case DataNode::Kind::String: {
        Value constant = IRBuilder::make(Opcode::String);
        constant.setString(leaf.as<DataNode::String>().value);
        return constant;
    }
    case DataNode::Kind::BigInt: {
        Value constant = IRBuilder::make(Opcode::BigInt);
        constant.setDigits(leaf.as<DataNode::BigInt>().digits);
        return constant;
    }
    case DataNode::Kind::Hole:
    case DataNode::Kind::Array:
    case DataNode::Kind::Object:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Value makeArray(const DataNode::Array& array, const OpenNode& closed, const FlowDataNodeStamp& stamp)
{
    Value allocation = IRBuilder::make(Opcode::NewArray);
    allocation.setContents(stamp.elements ? stamp.elements : array.contents);
    allocation.setHoles(closed.holes.span());
    allocation.setElementsOperands(closed.values.span());
    allocation.setAllocationSite(stamp.site);
    return allocation;
}

static Value makeDefinition(ValueID object, PropertyKeyID key, ValueID value)
{
    Value definition = IRBuilder::make(Opcode::PropDefine);
    definition.setObjectOperand(object);
    definition.setValueOperand(value);
    definition.setKey(key);
    return definition;
}

static bool isArrayIndex(const Program& program, PropertyKeyID key)
{
    return program.nameClassOf(key) == NameClass::Index;
}

static Value makeObject(const DataExpansion& expansion, const DataNode::Object& object, const OpenNode& closed, const FlowDataNodeStamp& stamp)
{
    const Program& program = expansion.program;
    const Layout& layout = program.at(object.layout);
    const Vector<DataMember>& members = object.members;
    auto convertToField = [&](size_t memberIndex, uint32_t field) {
        return convertTo(expansion.insertion, closed.values[memberIndex], layout.fields[field].declared);
    };

    // Array indexes are listed before the other own keys, which are listed in the order of their creation.
    Vector<uint32_t, 8> fields;
    Vector<ValueID, 8> values;
    Vector<size_t, 2> membersDefinedAfterHead;
    bool canAddToHead = true;
    for (size_t index = 0; index < members.size(); ++index) {
        std::optional<uint32_t> field = layout.findField(members[index].key);
        if (field && canAddToHead) {
            fields.append(*field);
            values.append(convertToField(index, *field));
            continue;
        }
        membersDefinedAfterHead.append(index);
        canAddToHead &= isArrayIndex(program, members[index].key);
    }

    Value allocation = IRBuilder::make(Opcode::NewObject);
    allocation.setLayout(object.layout);
    allocation.setFields(fields.span());
    allocation.setValuesOperands(values.span());
    allocation.setAllocationSite(stamp.site);
    allocation.setAllocationClass(stamp.allocationClass);
    allocation.setKeepsShape(stamp.keepsShape);
    if (membersDefinedAfterHead.isEmpty())
        return allocation;

    ValueID head = expansion.insertion.insert(WTF::move(allocation));
    for (size_t index : membersDefinedAfterHead) {
        std::optional<uint32_t> field = layout.findField(members[index].key);
        ValueID value = field ? convertToField(index, *field) : closed.values[index];
        expansion.insertion.insert(makeDefinition(head, members[index].key, value));
    }
    return IRBuilder::make(Opcode::FinishObject, { head });
}

static Value makeAggregate(const DataExpansion& expansion, const OpenNode& closed)
{
    FlowDataNodeStamp stamp = expansion.flow.dataNodeStamp(expansion.rootSite, closed.ordinal);
    if (const auto* array = closed.node->tryAs<DataNode::Array>())
        return makeArray(*array, closed, stamp);
    return makeObject(expansion, closed.node->as<DataNode::Object>(), closed, stamp);
}

static void addLeaf(const DataExpansion& expansion, OpenNode& parent, const DataNode& leaf)
{
    if (leaf.is<DataNode::Hole>()) {
        parent.holes.append(static_cast<uint32_t>(parent.numberOfFinishedChildren()));
        return;
    }
    parent.values.append(expansion.insertion.insert(makeConstant(leaf)));
}

static Value makeRootValue(const DataExpansion& expansion, const DataNode& root)
{
    if (!numberOfChildrenOf(root))
        return makeConstant(root);

    Value rootValue;
    Vector<OpenNode, 4> openNodes;
    forEachDataNodeInPreorder(root, [&](uint32_t ordinal, const DataNode& node) {
        if (std::optional<size_t> numberOfChildren = numberOfChildrenOf(node))
            openNodes.append(OpenNode { &node, ordinal, *numberOfChildren, { }, { } });
        else
            addLeaf(expansion, openNodes.last(), node);

        while (!openNodes.isEmpty() && openNodes.last().isComplete()) {
            OpenNode closed = openNodes.takeLast();
            Value aggregate = makeAggregate(expansion, closed);
            if (openNodes.isEmpty()) {
                rootValue = WTF::move(aggregate);
                return;
            }
            openNodes.last().values.append(expansion.insertion.insert(WTF::move(aggregate)));
        }
    });
    RELEASE_ASSERT(openNodes.isEmpty());
    return rootValue;
}

static void expandDataAt(FunctionPassContext& context, IRBuilder& builder, ProgramPoint point)
{
    Function& function = context.function;
    ValueID id = function.at(point.block).values[point.index];
    Origin origin = function.at(id).origin();
    AllocationSiteID rootSite = function.at(id).allocationSite();
    const DataNode& root = context.program.at(function.at(id).data()).root;

    // ScopedLambda hands its arguments to the functor as lvalues.
    auto insertBeforeData = [&](auto&& value) {
        return builder.insertBefore(point, WTF::move(value));
    };
    InsertValue insert = insertBeforeData;
    ValueInsertion insertion { builder.context(), function, context.types, origin, insert };
    builder.setOrigin(origin);

    Value replacement = makeRootValue({ context.program, context.inputs.flow, insertion, rootSite }, root);
    replacement.setOrigin(origin);
    replacement.type = resultTypeOf(context.program, function, replacement, context.types);
    replacement.setPrediction(predictionOf(context.program, function, replacement, context.types));
    function.at(id) = WTF::move(replacement);
}

PassOutcome expandData(FunctionPassContext& context)
{
    const Program& program = context.program;
    Function& function = context.function;
    uint64_t maxNodes = context.limit(LimitID::MaxExpandedDataNodes);
    IRBuilder builder(context.analyses.context(), function, context.types, &context.analyses);

    PassOutcome outcome = PassOutcome::Unchanged;
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        BlockID block(blockIndex);
        for (uint32_t index = static_cast<uint32_t>(function.at(block).values.size()); index--;) {
            const Value& value = function.at(function.at(block).values[index]);
            if (value.opcode != Opcode::Data)
                continue;
            if (!shouldExpand(summarize(program, program.at(value.data()).root), maxNodes))
                continue;
            expandDataAt(context, builder, { block, index });
            outcome = PassOutcome::Changed;
        }
    }
    builder.applyInsertions();
    return outcome;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
