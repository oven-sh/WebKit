/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFlowSummary.h"
#include "AOTFlowValue.h"
#include <array>
#include <initializer_list>
#include <optional>
#include <span>
#include <tuple>
#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/Forward.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Noncopyable.h>
#include <wtf/SegmentedVector.h>
#include <wtf/StdLibExtras.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

enum class FlowNodeKind : uint8_t {
    NativeCode,
    UnknownJavaScript,
    Variable,
    Heritage,
    DeclaredPart,
    Position,
    UndeclaredPart,
    PrototypeLink,
    IndexedProperties,
    StringProperties,
    SymbolProperties,
    Property,
    Getter,
    Setter,
    Temporary,
    Local,
};

enum class FlowOpenPart : uint8_t {
    PrototypeLink,
    IndexedProperties,
    StringProperties,
    SymbolProperties,
};

inline constexpr unsigned numberOfFlowOpenParts = 4;
static_assert(static_cast<unsigned>(FlowOpenPart::SymbolProperties) + 1 == numberOfFlowOpenParts);

constexpr FlowNodeKind nodeKindOf(FlowOpenPart part)
{
    switch (part) {
    case FlowOpenPart::PrototypeLink:
        return FlowNodeKind::PrototypeLink;
    case FlowOpenPart::IndexedProperties:
        return FlowNodeKind::IndexedProperties;
    case FlowOpenPart::StringProperties:
        return FlowNodeKind::StringProperties;
    case FlowOpenPart::SymbolProperties:
        return FlowNodeKind::SymbolProperties;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

constexpr bool belongsToObject(FlowNodeKind kind)
{
    switch (kind) {
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
        return true;
    case FlowNodeKind::NativeCode:
    case FlowNodeKind::UnknownJavaScript:
    case FlowNodeKind::Variable:
    case FlowNodeKind::Heritage:
    case FlowNodeKind::Temporary:
    case FlowNodeKind::Local:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

struct FlowNode {
    static constexpr uint32_t noOwner = UINT32_MAX;

    FlowValue value;
    FlowEdgeID firstEdge;
    uint32_t owner { noOwner };
};

static_assert(sizeof(FlowNode) == 32);

struct FlowEdge {
    FlowNodeID target;
    TypeID filter;
    FlowEdgeID next;
};

static_assert(sizeof(FlowEdge) == 12);

// A set of numbers other than zero.
class FlowNumberSet {
public:
    // False for a number that the set has.
    bool add(uint64_t number)
    {
        ASSERT(number);
        if ((m_size + 1) * 3 > m_slots.size() * 2)
            grow();
        uint64_t& slot = slotFor(number);
        if (slot)
            return false;
        slot = number;
        ++m_size;
        return true;
    }

private:
    uint64_t& slotFor(uint64_t number) LIFETIME_BOUND
    {
        size_t mask = m_slots.size() - 1;
        size_t index = hashOfNumbers(number, 0) & mask;
        while (m_slots[index] && m_slots[index] != number)
            index = (index + 1) & mask;
        return m_slots[index];
    }

    void grow();

    Vector<uint64_t> m_slots;
    size_t m_size { 0 };
};

struct FlowProperty {
    PropertyKeyID key;
    FlowNodeID value;
    FlowNodeID getter;
    FlowNodeID setter;
};

struct FlowCallSiteKey {
    FunctionID caller;
    ValueID call;
    FlowInstanceID outer;
    FlowContext context { FlowContext::Proven };
    // The caller stores on what the call gives, or hands it up to a caller that does. The other members decide this one.
    bool isAskedForNewObject { false };

    friend bool operator==(const FlowCallSiteKey&, const FlowCallSiteKey&) = default;
};

enum class FlowInstanceState : uint8_t {
    Created,
    Initialized,
    PastWorkLimit,
};

struct FlowInstance {
    FunctionID function;
    FlowContext context { FlowContext::Generic };
    FlowInstanceState state { FlowInstanceState::Created };
    FlowNodeID firstNode;
    std::optional<FlowCallSiteKey> callSite;

    bool isCallSiteInstance() const { return callSite.has_value(); }
};

struct FlowNodeDescription {
    FlowNodeKind kind { FlowNodeKind::Temporary };
    FlowInstanceID instance;
    FlowLocalID local;
    VariableID variable;
    LayoutID layout;
    FlowObjectID object;
    uint32_t index { 0 };
    PropertyKeyID key;
};

class FlowGraph {
    WTF_MAKE_NONCOPYABLE(FlowGraph);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(FlowGraph);
public:
    FlowGraph(const Program&, FlowObjectTable&, std::span<const FlowSummary> summariesByFunction, FlowValueTable&);

    const Program& program() const { return m_program; }
    const FlowObjectTable& objects() const { return m_objects; }
    FlowValueTable& values() { return m_values; }
    const FlowValueTable& values() const { return m_values; }

    FunctionID hostFunction() const { return FunctionID(m_program.functions.size()); }
    bool isHost(FunctionID function) const { return function == hostFunction(); }
    bool isHost(FlowInstanceID instance) const { return isHost(at(instance).function); }

    FlowInstanceID findInstance(FunctionID, FlowContext) const;
    FlowInstanceID createInstance(FunctionID, FlowContext);
    FlowInstanceID findCallSiteInstance(FunctionID, const FlowCallSiteKey&) const;
    FlowInstanceID createCallSiteInstance(FunctionID, const FlowCallSiteKey&);
    std::span<const FlowInstanceID> callSiteInstancesOf(FunctionID) const LIFETIME_BOUND;
    unsigned numberOfInstances() const { return m_instanceTable.instances.size(); }
    unsigned numberOfCallSiteInstances() const { return m_instanceTable.numberOfCallSiteInstances; }

    const FlowInstance& at(FlowInstanceID instance) const LIFETIME_BOUND { return m_instanceTable.instances[instance.index()].instance; }
    void setState(FlowInstanceID instance, FlowInstanceState state)
    {
        m_instanceTable.instances[instance.index()].instance.state = state;
        m_instanceTable.hasInstancesPastWorkLimit |= state == FlowInstanceState::PastWorkLimit;
    }

    bool hasInstancesPastWorkLimit() const { return m_instanceTable.hasInstancesPastWorkLimit; }
    const FlowSummary& summaryOf(FlowInstanceID instance) const LIFETIME_BOUND { return m_summaries[at(instance).function.index()]; }
    const FlowSummary& summaryOf(FunctionID function) const LIFETIME_BOUND { return m_summaries[function.index()]; }

    void forEachInstanceOf(FunctionID function, NOESCAPE const Invocable<void(FlowInstanceID)> auto& functor) const
    {
        for (FlowInstanceID instance : m_instanceTable.byFunction[function.index()]) {
            if (instance)
                functor(instance);
        }
        for (FlowInstanceID instance : callSiteInstancesOf(function))
            functor(instance);
    }

    bool isReached(FunctionID function) const
    {
        return findInstance(function, FlowContext::Generic) || isEnteredWithTrackedOperands(function);
    }

    bool isEnteredWithTrackedOperands(FunctionID function) const
    {
        return findInstance(function, FlowContext::Proven) || findInstance(function, FlowContext::ProvenUnderGeneric) || !callSiteInstancesOf(function).empty();
    }

    void setObjectsOf(FlowInstanceID, Vector<FlowObjectID>&& objectsBySite);
    FlowObjectID objectOf(FlowInstanceID, uint32_t siteInSummary) const;
    uint16_t& rangeGrowth(FlowInstanceID, uint32_t naturalOperationIndex) LIFETIME_BOUND;

    FlowNodeID nodeOf(FlowInstanceID, FlowLocalID) const;
    FlowNodeID nodeOf(FlowInstanceID, ValueID) const;
    FlowLocalID localOf(FlowNodeID) const;
    FlowInstanceID instanceOf(FlowNodeID) const;
    FlowObjectID objectOf(FlowNodeID) const;

    FlowNodeID nativeCode() const { return FlowNodeID(nativeCodeIndex); }
    FlowNodeID unknownJavaScript() const { return FlowNodeID(unknownJavaScriptIndex); }
    bool isUnseenCode(FlowNodeID node) const { return node.index() <= unknownJavaScriptIndex; }
    FlowNodeID variable(VariableID) const;
    FlowNodeID heritage(LayoutID) const;

    void didAppendObject(FlowObjectID);
    FlowNodeID declaredPart(FlowObjectID, uint32_t index) const;
    FlowNodeID field(FlowObjectID object, uint32_t fieldIndex) const { return declaredPart(object, fieldIndex); }
    FlowNodeID capture(FlowObjectID closure, uint32_t captureIndex) const { return declaredPart(closure, captureIndex); }
    FlowNodeID slot(FlowObjectID, BuiltinSlotID) const;
    bool holdsCollectionKeys(FlowNodeID) const;
    FlowNodeID position(FlowObjectID, uint32_t position) const;
    FlowNodeID undeclaredPart(FlowObjectID) const;
    FlowNodeID find(FlowObjectID, FlowOpenPart) const;
    FlowNodeID ensure(FlowObjectID, FlowOpenPart);

    const FlowProperty* findProperty(FlowObjectID, PropertyKeyID) const LIFETIME_BOUND;
    // Ascending by key. The list of the unknown object is in order of creation until it is sorted.
    std::span<const FlowProperty> propertiesOf(FlowObjectID) const LIFETIME_BOUND;
    void sortPropertiesOfUnknownObject();
    FlowNodeID ensurePropertyValue(FlowObjectID, PropertyKeyID, bool& isNew);
    FlowNodeID ensureGetter(FlowObjectID, PropertyKeyID, bool& isNew);
    FlowNodeID ensureSetter(FlowObjectID, PropertyKeyID, bool& isNew);

    void forEachNodeOf(FlowObjectID object, NOESCAPE const Invocable<void(FlowNodeID)> auto& functor) const
    {
        const ObjectNodes& nodes = m_objectNodes[object.index()];
        for (uint32_t index = 0, count = m_objects.at(object).numberOfParts(); index < count; ++index)
            functor(FlowNodeID(nodes.firstPart.index() + index));
        for (FlowNodeID node : nodes.openParts) {
            if (node)
                functor(node);
        }
        for (size_t index = 0; index < nodes.properties.size(); ++index) {
            FlowProperty property = nodes.properties[index];
            for (FlowNodeID node : { property.value, property.getter, property.setter }) {
                if (node && kindOf(node) != FlowNodeKind::DeclaredPart)
                    functor(node);
            }
        }
    }

    FlowNodeID addTemporary(FlowInstanceID chargedInstance = { });

    unsigned numberOfNodes() const { return m_nodes.size(); }
    unsigned numberOfEdges() const { return m_edges.size(); }
    FlowNode& at(FlowNodeID node) LIFETIME_BOUND { return m_nodes[node.index()]; }
    const FlowNode& at(FlowNodeID node) const LIFETIME_BOUND { return m_nodes[node.index()]; }
    const FlowValue& valueOf(FlowNodeID node) const LIFETIME_BOUND { return m_nodes[node.index()].value; }
    FlowNodeKind kindOf(FlowNodeID node) const { return m_kinds[node.index()]; }

    bool addEdge(FlowNodeID from, FlowNodeID to, TypeID filter = { });
    bool hasEdgeToUnseenCode(FlowNodeID from) const { return m_nodesWithEdgesToUnseenCode.get(from.index()); }

    void forEachEdge(FlowNodeID from, NOESCAPE const Invocable<void(const FlowEdge&)> auto& functor) const
    {
        FlowEdgeID next = m_nodes[from.index()].firstEdge;
        while (next) {
            FlowEdge edge = m_edges[next.index()];
            next = edge.next;
            functor(edge);
        }
    }

    void forEachSuccessor(FlowNodeID from, NOESCAPE const Invocable<void(FlowNodeID target, TypeID filter)> auto& functor) const
    {
        if (kindOf(from) == FlowNodeKind::Local) {
            const InstanceData& data = m_instanceTable.instances[m_nodes[from.index()].owner];
            const FlowSummary& summary = m_summaries[data.instance.function.index()];
            uint32_t firstNode = data.instance.firstNode.index();
            for (FlowLocalID target : summary.successors.of(FlowLocalID(from.index() - firstNode)))
                functor(summary.isExternal(target) ? data.externals[target.index() - summary.numberOfLocals] : FlowNodeID(firstNode + target.index()), TypeID());
        }
        forEachEdge(from, [&](const FlowEdge& edge) {
            functor(edge.target, edge.filter);
        });
    }

    // The successors, and where a filter that keeps what is outside a type copies to.
    void forEachCopyTarget(FlowNodeID from, NOESCAPE const Invocable<void(FlowNodeID)> auto& functor) const
    {
        forEachSuccessor(from, [&](FlowNodeID target, TypeID) {
            functor(target);
        });
        if (kindOf(from) != FlowNodeKind::Local)
            return;

        FlowInstanceID instance = instanceOf(from);
        const FlowSummary& summary = summaryOf(instance);
        for (FlowStatementReference statement : summary.waiters.of(localOf(from))) {
            if (statement.kind != FlowStatementKind::Filter)
                continue;
            const FlowFilter& filter = summary.statements.filters[statement.index];
            if (!filter.isEdge())
                functor(nodeOf(instance, filter.to));
        }
    }

    TypeID typeAsRead(FlowNodeID) const;
    FunctionID functionChargedFor(FlowNodeID) const;
    FlowNodeDescription describe(FlowNodeID) const;
    Witness witnessOf(FlowInstanceID, ValueID origin) const;
    void dump(PrintStream&) const;
    void dumpNameOf(PrintStream&, FlowNodeID) const;

private:
    static constexpr uint32_t nativeCodeIndex = 0;
    static constexpr uint32_t unknownJavaScriptIndex = 1;

    struct ObjectNodes {
        FlowNodeID firstPart;
        std::array<FlowNodeID, numberOfFlowOpenParts> openParts;
        Vector<FlowProperty> properties;
    };

    // Every name of every escaped object gets an entry on the unknown object, too many to keep a vector sorted at each insertion.
    struct UnknownObjectProperties {
        Vector<uint32_t> positionPlusOneOfKey;
        bool isSorted { false };
    };

    struct InstanceData {
        FlowInstance instance;
        Vector<FlowObjectID> objects;
        Vector<FlowNodeID> externals;
        Vector<uint16_t> rangeGrowth;
    };

    struct InstanceTable {
        SegmentedVector<InstanceData, 256> instances;
        Vector<std::array<FlowInstanceID, numberOfFlowContexts>> byFunction;
        HashMap<std::tuple<FunctionID, FunctionID, ValueID, FlowInstanceID, uint8_t>, FlowInstanceID> byCallSite;
        Vector<Vector<FlowInstanceID>> callSiteInstancesByFunction;
        unsigned numberOfCallSiteInstances { 0 };
        bool hasInstancesPastWorkLimit { false };
    };

    FlowNodeID appendNodes(FlowNodeKind, unsigned count, uint32_t owner);
    FlowInstanceID appendInstance(FunctionID, FlowContext, std::optional<FlowCallSiteKey>);
    FlowNodeID resolve(const FlowExternalNode&, std::span<const FlowObjectID> objectsBySite);
    FlowProperty& ensureProperty(FlowObjectID, PropertyKeyID) LIFETIME_BOUND;
    FlowNodeID ensureAccessor(FlowObjectID, PropertyKeyID, FlowNodeKind, FlowNodeID FlowProperty::* accessor, bool& isNew);
    bool addEdgeOfNodeWithManyEdges(FlowNodeID from, FlowNodeID to, TypeID filter);

    const Program& m_program;
    FlowObjectTable& m_objects;
    std::span<const FlowSummary> m_summaries;
    FlowValueTable& m_values;

    SegmentedVector<FlowNode, 1024> m_nodes;
    Vector<FlowNodeKind> m_kinds;
    Vector<FlowEdge> m_edges;
    // The edges of a node with few of them are compared one by one.
    BitVector m_nodesWithManyEdges;
    BitVector m_nodesWithEdgesToUnseenCode;
    FlowNumberSet m_plainEdgesOfNodesWithManyEdges;
    HashSet<std::tuple<FlowNodeID, FlowNodeID, TypeID>> m_filteredEdgesOfNodesWithManyEdges;

    FlowNodeID m_firstVariable;
    FlowNodeID m_firstHeritage;
    size_t m_numberOfHeritageNodes { 0 };
    SegmentedVector<ObjectNodes, 1024> m_objectNodes;
    UnknownObjectProperties m_unknownObjectProperties;
    InstanceTable m_instanceTable;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
