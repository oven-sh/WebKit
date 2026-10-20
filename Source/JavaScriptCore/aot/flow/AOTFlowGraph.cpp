/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowGraph.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTFlowObjectOperations.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static FlowNodeID nodeInBlock(FlowNodeID first, size_t count, uint32_t index)
{
    RELEASE_ASSERT(index < count);
    return FlowNodeID(first.index() + index);
}

static FlowNodeID findFieldNode(const FlowGraph& graph, FlowObjectID object, PropertyKeyID key)
{
    FlowFieldLocation location = findFieldLocation(graph, object, key);
    return location.lookup == FlowFieldLookup::Found ? location.node : FlowNodeID();
}

FlowGraph::FlowGraph(const Program& program, FlowObjectTable& objects, std::span<const FlowSummary> summariesByFunction, FlowValueTable& values)
    : m_program(program)
    , m_objects(objects)
    , m_summaries(summariesByFunction)
    , m_values(values)
{
    RELEASE_ASSERT(m_summaries.size() == program.functions.size() + 1);

    size_t numberOfNodes = 2 + program.variables.size() + program.layouts.size();
    for (uint32_t index = 0; index < objects.size(); ++index)
        numberOfNodes += objects.at(FlowObjectID(index)).numberOfParts();
    for (const FlowSummary& summary : m_summaries)
        numberOfNodes += summary.numberOfLocals;
    m_kinds.reserveInitialCapacity(numberOfNodes);

    auto appendNodePerIndex = [&](FlowNodeKind kind, size_t count) {
        FlowNodeID first(m_nodes.size());
        for (uint32_t index = 0; index < count; ++index)
            appendNodes(kind, 1, index);
        return first;
    };

    appendNodes(FlowNodeKind::NativeCode, 1, FlowNode::noOwner);
    appendNodes(FlowNodeKind::UnknownJavaScript, 1, FlowNode::noOwner);
    RELEASE_ASSERT(kindOf(nativeCode()) == FlowNodeKind::NativeCode && kindOf(unknownJavaScript()) == FlowNodeKind::UnknownJavaScript);
    m_firstVariable = appendNodePerIndex(FlowNodeKind::Variable, program.variables.size());
    m_numberOfHeritageNodes = program.layouts.size();
    m_firstHeritage = appendNodePerIndex(FlowNodeKind::Heritage, m_numberOfHeritageNodes);
    for (uint32_t index = 0; index < objects.size(); ++index)
        didAppendObject(FlowObjectID(index));

    m_instanceTable.byFunction.grow(program.functions.size() + 1);
    m_instanceTable.callSiteInstancesByFunction.grow(program.functions.size() + 1);
}

FlowNodeID FlowGraph::appendNodes(FlowNodeKind kind, unsigned count, uint32_t owner)
{
    size_t first = m_nodes.size();
    RELEASE_ASSERT(first + count < UINT32_MAX - 1);
    if (!count)
        return FlowNodeID(first);

    m_nodes.grow(first + count);
    for (size_t index = first; index < first + count; ++index)
        m_nodes[index].owner = owner;
    m_kinds.insertFill(first, kind, count);
    return FlowNodeID(first);
}

FlowInstanceID FlowGraph::instanceOf(FlowNodeID node) const
{
    switch (kindOf(node)) {
    case FlowNodeKind::Local:
    case FlowNodeKind::Temporary: {
        uint32_t owner = at(node).owner;
        return owner == FlowNode::noOwner ? FlowInstanceID() : FlowInstanceID(owner);
    }
    case FlowNodeKind::NativeCode:
    case FlowNodeKind::UnknownJavaScript:
    case FlowNodeKind::Variable:
    case FlowNodeKind::Heritage:
    case FlowNodeKind::DeclaredPart:
    case FlowNodeKind::Position:
    case FlowNodeKind::UndeclaredPart:
    case FlowNodeKind::PrototypeLink:
    case FlowNodeKind::IndexedProperties:
    case FlowNodeKind::StringProperties:
    case FlowNodeKind::SymbolProperties:
    case FlowNodeKind::Property:
    case FlowNodeKind::Getter:
    case FlowNodeKind::Setter:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

FlowObjectID FlowGraph::objectOf(FlowNodeID node) const
{
    return belongsToObject(kindOf(node)) ? FlowObjectID(at(node).owner) : FlowObjectID();
}

FlowNodeID FlowGraph::variable(VariableID variable) const
{
    return nodeInBlock(m_firstVariable, m_program.variables.size(), variable.index());
}

FlowNodeID FlowGraph::heritage(LayoutID layout) const
{
    return nodeInBlock(m_firstHeritage, m_numberOfHeritageNodes, layout.index());
}

void FlowGraph::didAppendObject(FlowObjectID object)
{
    RELEASE_ASSERT(object.index() == m_objectNodes.size());

    const FlowObject& record = m_objects.at(object);
    FlowNodeID firstPart = appendNodes(FlowNodeKind::DeclaredPart, record.numberOfDeclaredParts, object.index());
    appendNodes(FlowNodeKind::Position, record.numberOfPositions, object.index());
    if (record.hasUndeclaredPart)
        appendNodes(FlowNodeKind::UndeclaredPart, 1, object.index());
    m_objectNodes.append(ObjectNodes { firstPart, { }, { } });
}

FlowNodeID FlowGraph::declaredPart(FlowObjectID object, uint32_t index) const
{
    return nodeInBlock(m_objectNodes[object.index()].firstPart, m_objects.at(object).numberOfDeclaredParts, index);
}

FlowNodeID FlowGraph::slot(FlowObjectID object, BuiltinSlotID slotID) const
{
    const FlowObject& record = m_objects.at(object);
    if (!slotID || !record.builtinClass)
        return { };

    const BuiltinTable& builtins = m_program.builtins();
    const BuiltinSlot& slot = builtins.at(slotID);
    if (!builtins.isSameOrSubclassOf(record.builtinClass, slot.owner))
        return { };
    if (slot.indexInClass >= record.numberOfDeclaredParts)
        return { };
    return declaredPart(object, slot.indexInClass);
}

bool FlowGraph::holdsCollectionKeys(FlowNodeID node) const
{
    if (kindOf(node) != FlowNodeKind::DeclaredPart)
        return false;
    FlowObjectID object = objectOf(node);
    return node == slot(object, BuiltinSlots::MapKey) || node == slot(object, BuiltinSlots::SetElement);
}

FlowNodeID FlowGraph::position(FlowObjectID object, uint32_t position) const
{
    const FlowObject& record = m_objects.at(object);
    FlowNodeID firstPosition(m_objectNodes[object.index()].firstPart.index() + record.numberOfDeclaredParts);
    return nodeInBlock(firstPosition, record.numberOfPositions, position);
}

FlowNodeID FlowGraph::undeclaredPart(FlowObjectID object) const
{
    const FlowObject& record = m_objects.at(object);
    if (!record.hasUndeclaredPart)
        return { };
    return FlowNodeID(m_objectNodes[object.index()].firstPart.index() + record.numberOfDeclaredParts + record.numberOfPositions);
}

FlowNodeID FlowGraph::find(FlowObjectID object, FlowOpenPart part) const
{
    return m_objectNodes[object.index()].openParts[static_cast<size_t>(part)];
}

FlowNodeID FlowGraph::ensure(FlowObjectID object, FlowOpenPart part)
{
    FlowNodeID& node = m_objectNodes[object.index()].openParts[static_cast<size_t>(part)];
    if (!node)
        node = appendNodes(nodeKindOf(part), 1, object.index());
    return node;
}

const FlowProperty* FlowGraph::findProperty(FlowObjectID object, PropertyKeyID key) const
{
    const Vector<FlowProperty>& properties = m_objectNodes[object.index()].properties;
    if (object == unknownFlowObject && !m_unknownObjectProperties.isSorted) {
        const Vector<uint32_t>& positions = m_unknownObjectProperties.positionPlusOneOfKey;
        uint32_t positionPlusOne = key.index() < positions.size() ? positions[key.index()] : 0;
        return positionPlusOne ? &properties[positionPlusOne - 1] : nullptr;
    }

    auto found = std::ranges::lower_bound(properties, key, { }, &FlowProperty::key);
    if (found == properties.end() || found->key != key)
        return nullptr;
    return found;
}

std::span<const FlowProperty> FlowGraph::propertiesOf(FlowObjectID object) const
{
    return m_objectNodes[object.index()].properties.span();
}

FlowProperty& FlowGraph::ensureProperty(FlowObjectID object, PropertyKeyID key)
{
    RELEASE_ASSERT(key);
    Vector<FlowProperty>& properties = m_objectNodes[object.index()].properties;
    if (object == unknownFlowObject && !m_unknownObjectProperties.isSorted) {
        Vector<uint32_t>& positions = m_unknownObjectProperties.positionPlusOneOfKey;
        if (positions.size() <= key.index())
            positions.insertFill(positions.size(), 0u, std::max<size_t>(key.index() + 1, positions.size() * 2) - positions.size());

        uint32_t& positionPlusOne = positions[key.index()];
        if (!positionPlusOne) {
            properties.append(FlowProperty { key, { }, { }, { } });
            positionPlusOne = static_cast<uint32_t>(properties.size());
        }
        return properties[positionPlusOne - 1];
    }

    auto found = std::ranges::lower_bound(properties, key, { }, &FlowProperty::key);
    size_t position = found - properties.begin();
    if (found == properties.end() || found->key != key)
        properties.insert(position, FlowProperty { key, { }, { }, { } });
    return properties[position];
}

void FlowGraph::sortPropertiesOfUnknownObject()
{
    std::ranges::sort(m_objectNodes[unknownFlowObject.index()].properties, { }, &FlowProperty::key);
    m_unknownObjectProperties.positionPlusOneOfKey.clear();
    m_unknownObjectProperties.isSorted = true;
}

FlowNodeID FlowGraph::ensurePropertyValue(FlowObjectID object, PropertyKeyID key, bool& isNew)
{
    isNew = false;
    if (FlowNodeID fieldNode = findFieldNode(*this, object, key))
        return fieldNode;

    FlowNodeID& value = ensureProperty(object, key).value;
    if (!value) {
        value = appendNodes(FlowNodeKind::Property, 1, object.index());
        isNew = true;
    }
    return value;
}

FlowNodeID FlowGraph::ensureAccessor(FlowObjectID object, PropertyKeyID key, FlowNodeKind kind, FlowNodeID FlowProperty::* accessor, bool& isNew)
{
    isNew = false;
    FlowProperty& property = ensureProperty(object, key);
    bool isNewEntry = !property.value && !property.getter && !property.setter;
    if (isNewEntry)
        property.value = findFieldNode(*this, object, key);

    FlowNodeID& node = property.*accessor;
    if (!node) {
        node = appendNodes(kind, 1, object.index());
        isNew = true;
    }
    return node;
}

FlowNodeID FlowGraph::ensureGetter(FlowObjectID object, PropertyKeyID key, bool& isNew)
{
    return ensureAccessor(object, key, FlowNodeKind::Getter, &FlowProperty::getter, isNew);
}

FlowNodeID FlowGraph::ensureSetter(FlowObjectID object, PropertyKeyID key, bool& isNew)
{
    return ensureAccessor(object, key, FlowNodeKind::Setter, &FlowProperty::setter, isNew);
}

FlowNodeID FlowGraph::addTemporary(FlowInstanceID chargedInstance)
{
    return appendNodes(FlowNodeKind::Temporary, 1, chargedInstance ? chargedInstance.index() : FlowNode::noOwner);
}

void FlowNumberSet::grow()
{
    constexpr size_t initialNumberOfSlots = 64;
    Vector<uint64_t> oldSlots = std::exchange(m_slots, Vector<uint64_t>(FillWith { }, std::max(initialNumberOfSlots, m_slots.size() * 2), uint64_t { 0 }));
    for (uint64_t number : oldSlots) {
        if (number)
            slotFor(number) = number;
    }
}

bool FlowGraph::addEdgeOfNodeWithManyEdges(FlowNodeID from, FlowNodeID to, TypeID filter)
{
    if (filter)
        return m_filteredEdgesOfNodesWithManyEdges.add(std::tuple { from, to, filter }).isNewEntry;
    return m_plainEdgesOfNodesWithManyEdges.add((static_cast<uint64_t>(from.index()) + 1) << 32 | to.index());
}

bool FlowGraph::addEdge(FlowNodeID from, FlowNodeID to, TypeID filter)
{
    RELEASE_ASSERT(from && to);
    if (from == to && !filter)
        return false;

    constexpr unsigned maxEdgesToCompare = 8;
    FlowNode& node = at(from);
    if (!m_nodesWithManyEdges.get(from.index())) {
        unsigned numberOfEdges = 0;
        for (FlowEdgeID next = node.firstEdge; next; next = m_edges[next.index()].next) {
            const FlowEdge& edge = m_edges[next.index()];
            if (edge.target == to && edge.filter == filter)
                return false;
            ++numberOfEdges;
        }
        if (numberOfEdges == maxEdgesToCompare) {
            if (from.index() >= m_nodesWithManyEdges.size())
                m_nodesWithManyEdges.ensureSize(std::max<size_t>(m_nodes.size(), m_nodesWithManyEdges.size() * 2));
            m_nodesWithManyEdges.quickSet(from.index());
            forEachEdge(from, [&](const FlowEdge& edge) {
                addEdgeOfNodeWithManyEdges(from, edge.target, edge.filter);
            });
        }
    }
    if (m_nodesWithManyEdges.get(from.index()) && !addEdgeOfNodeWithManyEdges(from, to, filter))
        return false;

    if (to == nativeCode() || to == unknownJavaScript()) {
        if (from.index() >= m_nodesWithEdgesToUnseenCode.size())
            m_nodesWithEdgesToUnseenCode.ensureSize(std::max<size_t>(m_nodes.size(), m_nodesWithEdgesToUnseenCode.size() * 2));
        m_nodesWithEdgesToUnseenCode.quickSet(from.index());
    }

    FlowEdgeID edge(m_edges.size());
    m_edges.append(FlowEdge { to, filter, node.firstEdge });
    node.firstEdge = edge;
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
