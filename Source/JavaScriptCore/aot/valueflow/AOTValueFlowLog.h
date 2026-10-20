/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTTypeTable.h"
#include "AOTValueFlow.h"
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

enum class ValueFlowMadeNodeKind : uint8_t {
    // Holds the set of its key for ever: what a rule adds to a node is an edge out of one of these.
    Constant,
    Read,
    Written,
    Lookup,
    CallGroup,
    Hub,
    Masked,
    // Holds the objects that have one mark.
    Marked,
    // Holds what another node holds of the members of a set, and no member stands for another here.
    Part,
    // Holds all that is written through a set under the names of one kind; hands on to all that is read through a set under the names of one kind.
    // Names of different kinds that may be the same meet here, so a read of every property is linked set by set and not name by name.
    WrittenUnderKind,
    ReadUnderKind,
};

// What a node that a rule asks for is found again by.
struct ValueFlowNodeKey {
    static ValueFlowNodeKey constant(ValueSetID set) { return { ValueFlowMadeNodeKind::Constant, 0, 0, set }; }
    static ValueFlowNodeKey named(ValueFlowMadeNodeKind kind, ValueSetID set, ValueFlowName name) { return { kind, static_cast<uint8_t>(name.kind), name.payload, set }; }
    static ValueFlowNodeKey underKind(ValueFlowMadeNodeKind kind, ValueSetID set, ValueFlowNameKind nameKind) { return { kind, static_cast<uint8_t>(nameKind), 0, set }; }
    static ValueFlowNodeKey callGroup(ValueSetID callees, ValueFlowCallKind kind) { return { ValueFlowMadeNodeKind::CallGroup, static_cast<uint8_t>(kind), 0, callees }; }
    static ValueFlowNodeKey hub(ValueFlowBehaviorID behavior) { return { ValueFlowMadeNodeKind::Hub, 0, behavior.index(), { } }; }
    static ValueFlowNodeKey masked(ValueFlowNodeID node, ValueSetID mask) { return { ValueFlowMadeNodeKind::Masked, 0, node.index(), mask }; }
    static ValueFlowNodeKey part(ValueFlowNodeID node, ValueSetID within) { return { ValueFlowMadeNodeKind::Part, 0, node.index(), within }; }
    static ValueFlowNodeKey marked(ObjectMutation mutation) { return { ValueFlowMadeNodeKind::Marked, 0, static_cast<uint32_t>(mutation), { } }; }

    ValueFlowName name() const { return { static_cast<ValueFlowNameKind>(detail), payload }; }
    ValueFlowCallKind callKind() const { return static_cast<ValueFlowCallKind>(detail); }
    ValueFlowBehaviorID behavior() const { return ValueFlowBehaviorID(payload); }
    ValueFlowNodeID maskedNode() const { return ValueFlowNodeID(payload); }

    unsigned numberOfNodes() const { return kind == ValueFlowMadeNodeKind::CallGroup ? numberOfNodesOfCallGroup : 1; }
    uint32_t hash() const
    {
        uint64_t hash = (static_cast<uint64_t>(payload) << 32 | set.index()) * 0x9e3779b97f4a7c15ull + (static_cast<uint32_t>(kind) << 8 | detail) * 0xbf58476d1ce4e5b9ull;
        return static_cast<uint32_t>(hash >> 32 ^ hash);
    }

    friend bool operator==(const ValueFlowNodeKey&, const ValueFlowNodeKey&) = default;

    static constexpr unsigned numberOfNodesOfCallGroup = 5 + numberOfGroupedArguments;

    ValueFlowMadeNodeKind kind { ValueFlowMadeNodeKind::Constant };
    uint8_t detail { 0 };
    uint32_t payload { 0 };
    ValueSetID set;
};

inline ValueFlowCallGroup callGroupAt(ValueFlowNodeID first)
{
    auto at = [&](unsigned offset) {
        return ValueFlowNodeID(first.index() + offset);
    };
    return { at(0), at(1), at(5), at(2), at(3), at(4) };
}

// What the rules that one thread has run in one round have added. Nothing in it depends on what the other threads did.
// A node that no earlier round has made gets a number of the log's own; the merge of the logs gives it its number in the graph.
class ValueFlowRoundLog {
    WTF_MAKE_NONCOPYABLE(ValueFlowRoundLog);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ValueFlowRoundLog);
public:
    static constexpr uint32_t numberedByLog = 1u << 31;

    struct MadeNodes {
        ValueFlowNodeKey key;
        uint32_t first { 0 };
    };

    struct Use {
        BuiltinID builtin;
        OptionSet<BuiltinUse> uses;
    };

    // The inputs of a run are those between the end of the run before it and its own end.
    struct Run {
        ValueFlowStatementID statement;
        uint32_t endOfNodesRead { 0 };
        uint32_t endOfFunctionsAsked { 0 };
        uint32_t endOfNodesAskedFor { 0 };
        uint32_t endOfEdges { 0 };
        uint32_t endOfReached { 0 };
        // The edges that the run has asked for, those that an earlier round had added among them.
        uint32_t numberOfConnections { 0 };
        uint32_t endOfMade { 0 };
    };

    ValueFlowRoundLog() = default;

    static bool isNumberedByLog(ValueFlowNodeID node) { return node.index() & numberedByLog; }

    ValueFlowNodeID nodeFor(const ValueFlowNodeKey& key)
    {
        uint32_t hash = key.hash();
        std::optional<uint32_t> existing = m_madeByKey.find(hash, [&](uint32_t position) {
            return made[position].key == key;
        });
        if (existing)
            return ValueFlowNodeID(made[*existing].first | numberedByLog);
        m_madeByKey.add(hash, made.size());
        made.append(MadeNodes { key, numberOfMadeNodes });
        numberOfMadeNodes += key.numberOfNodes();
        return ValueFlowNodeID(made.last().first | numberedByLog);
    }

    void clear()
    {
        made.shrink(0);
        numberOfMadeNodes = 0;
        m_madeByKey = HashedIndex();
        edges.shrink(0);
        reached.shrink(0);
        uses.shrink(0);
        runs.shrink(0);
        nodesRead.shrink(0);
        functionsAsked.shrink(0);
        nodesAskedFor.shrink(0);
    }

    Vector<MadeNodes> made;
    uint32_t numberOfMadeNodes { 0 };
    Vector<ValueFlowEdge> edges;
    // An edge past this many is dropped, and the solve that the log belongs to has no result.
    uint64_t mostEdges { UINT64_MAX };
    bool hasDroppedEdges { false };
    uint32_t numberOfConnectionsOfRun { 0 };
    Vector<FunctionID> reached;
    Vector<Use> uses;
    Vector<Run> runs;
    Vector<ValueFlowNodeID> nodesRead;
    Vector<FunctionID> functionsAsked;
    // The first node of what read(), written(), lookup(), group(), hub() and masked() have answered.
    Vector<ValueFlowNodeID> nodesAskedFor;

private:
    HashedIndex m_madeByKey;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
