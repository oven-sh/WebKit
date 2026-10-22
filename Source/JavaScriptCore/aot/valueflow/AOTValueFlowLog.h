/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTTypeTable.h"
#include "AOTValueFlow.h"
#include <optional>
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
    // Holds all that is written through a set under the names of one kind; hands on to all that is read through a set under the names of one kind.
    // Names of different kinds that may be the same meet here, so a read of every property is linked set by set and not name by name.
    WrittenUnderKind,
    ReadUnderKind,
    // What a node of a body holds for some of the calls of its function, and the markers of those calls: see ValueFlowInstancedFunctions.
    Instance,
};

// Names under which most objects never get anything: see ValueFlowRuleContext::mayFindSomethingWritten().
enum class ValueFlowRareWrite : uint8_t {
    Accessor,
    Deletion,
    Copy,
    PrototypeLink,
};
constexpr unsigned numberOfValueFlowRareWrites = 4;

constexpr std::optional<ValueFlowRareWrite> rareWriteFoundUnder(ValueFlowNameKind kind)
{
    switch (kind) {
    case ValueFlowNameKind::Getter:
    case ValueFlowNameKind::Setter:
    case ValueFlowNameKind::GetterUnderUnknownKey:
    case ValueFlowNameKind::SetterUnderUnknownKey:
    case ValueFlowNameKind::EveryGetter:
    case ValueFlowNameKind::EverySetter:
        return ValueFlowRareWrite::Accessor;
    case ValueFlowNameKind::DeletedMember:
    case ValueFlowNameKind::DeletedMemberUnderUnknownKey:
        return ValueFlowRareWrite::Deletion;
    case ValueFlowNameKind::CopiedFrom:
        return ValueFlowRareWrite::Copy;
    case ValueFlowNameKind::PrototypeLink:
        return ValueFlowRareWrite::PrototypeLink;
    default:
        return std::nullopt;
    }
}

// What a node that a rule asks for is found again by.
struct ValueFlowNodeKey {
    static ValueFlowNodeKey constant(ValueSetID set) { return { ValueFlowMadeNodeKind::Constant, 0, 0, set }; }
    // A constant through which a rule brings the unknown object into being has one node for each source: whoever walks back along the edges finds the source where the walk ends.
    static ValueFlowNodeKey constant(ValueSetID set, ValueFlowUnknownSource source) { return { ValueFlowMadeNodeKind::Constant, static_cast<uint8_t>(static_cast<uint8_t>(source) + 1), 0, set }; }
    static ValueFlowNodeKey named(ValueFlowMadeNodeKind kind, ValueSetID set, ValueFlowName name) { return { kind, static_cast<uint8_t>(name.kind), name.payload, set }; }
    static ValueFlowNodeKey underKind(ValueFlowMadeNodeKind kind, ValueSetID set, ValueFlowNameKind nameKind) { return { kind, static_cast<uint8_t>(nameKind), 0, set }; }
    // With a marker: the group of the calls that enter the instance of the callee with that marker.
    static ValueFlowNodeKey callGroup(ValueSetID callees, ValueFlowCallKind kind, ValueFlowObjectID marker = { }) { return { ValueFlowMadeNodeKind::CallGroup, static_cast<uint8_t>(kind), marker ? marker.index() + 1 : 0, callees }; }
    static ValueFlowNodeKey instance(ValueFlowNodeID node, ValueSetID markerAlone) { return { ValueFlowMadeNodeKind::Instance, 0, node.index(), markerAlone }; }
    static ValueFlowNodeKey markersOf(FunctionID function) { return { ValueFlowMadeNodeKind::Instance, 1, function.index(), { } }; }
    static ValueFlowNodeKey restArgument(FunctionID function, unsigned index, ValueSetID markerAlone) { return { ValueFlowMadeNodeKind::Instance, static_cast<uint8_t>(2 + index), function.index(), markerAlone }; }
    static ValueFlowNodeKey hub(ValueFlowBehaviorID behavior) { return { ValueFlowMadeNodeKind::Hub, 0, behavior.index(), { } }; }
    static ValueFlowNodeKey masked(ValueFlowNodeID node, ValueSetID mask, ValueFlowMaskKind kind = ValueFlowMaskKind::Plain) { return { ValueFlowMadeNodeKind::Masked, static_cast<uint8_t>(kind), node.index(), mask }; }
    static ValueFlowNodeKey marked(ObjectMutation mutation) { return { ValueFlowMadeNodeKind::Marked, 0, static_cast<uint32_t>(mutation), { } }; }
    // Two marks that only rules ask after; no result reads them. Their payloads lie above those of the mutations.
    // The objects of the program that are more than their properties: somebody has written an accessor, a deletion, another form of a property or a copy through a set that lists them,
    // or has given them the mark DeleteField, RedefineProperty or ChangePrototype. No placeholder: see ValueFlowRuleContext::isPlainSoFar().
    static ValueFlowNodeKey objectsThatAreNotPlain() { return { ValueFlowMadeNodeKind::Marked, 0, 1u << 16, { } }; }
    // Those through which somebody has stored or defined under a key that the analysis does not know.
    static ValueFlowNodeKey objectsWrittenUnderUnknownKey() { return { ValueFlowMadeNodeKind::Marked, 0, (1u << 16) + 1, { } }; }
    static ValueFlowNodeKey objectsWithRareWrite(ValueFlowRareWrite write) { return { ValueFlowMadeNodeKind::Marked, 0, (1u << 16) + 2 + static_cast<uint32_t>(write), { } }; }

    ValueFlowName name() const { return { static_cast<ValueFlowNameKind>(detail), payload }; }
    ValueFlowCallKind callKind() const { return static_cast<ValueFlowCallKind>(detail); }
    ValueFlowObjectID markerOfCallGroup() const { return payload ? ValueFlowObjectID(payload - 1) : ValueFlowObjectID(); }
    bool isInstanceOfNode() const { return kind == ValueFlowMadeNodeKind::Instance && !detail; }
    ValueFlowNodeID nodeOfInstance() const { return ValueFlowNodeID(payload); }
    ValueFlowBehaviorID behavior() const { return ValueFlowBehaviorID(payload); }
    ValueFlowNodeID maskedNode() const { return ValueFlowNodeID(payload); }
    ValueFlowMaskKind maskKind() const { return static_cast<ValueFlowMaskKind>(detail); }
    // Of a constant: none where no rule has named a source.
    std::optional<ValueFlowUnknownSource> unknownSource() const { return detail ? std::optional(static_cast<ValueFlowUnknownSource>(detail - 1)) : std::nullopt; }

    unsigned numberOfNodes() const { return kind == ValueFlowMadeNodeKind::CallGroup ? numberOfNodesOfCallGroup : 1; }
    uint32_t hash() const
    {
        uint64_t hash = (static_cast<uint64_t>(payload) << 32 | set.index()) * 0x9e3779b97f4a7c15ull;
        hash = (hash ^ hash >> 32 ^ (static_cast<uint32_t>(kind) << 8 | detail)) * 0xbf58476d1ce4e5b9ull;
        return static_cast<uint32_t>(hash >> 32);
    }

    friend bool operator==(const ValueFlowNodeKey&, const ValueFlowNodeKey&) = default;

    static constexpr unsigned numberOfNodesOfCallGroup = 6 + numberOfGroupedArguments;

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
    return { at(0), at(1), at(6), at(2), at(5), at(3), at(4) };
}

// What the rules that one thread has run in one round have added. Nothing in it depends on what the other threads did.
// Most nodes that no earlier turn has made are made where a rule asks for them: ValueFlowSolver::makeNodesWhereAskedFor(). One that is not gets a number of the log's own, and its number in the graph at the end of the turn.
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

    // Numbers in a row that are this thread's to give, so that no two threads take theirs from one counter or write side by side, and what one statement asks for stands in a row.
    // What is left of them at the end of a turn is given in the next one.
    struct NumbersToGive {
        uint32_t next { 0 };
        uint32_t end { 0 };
        // A thread that makes little holds little back.
        uint32_t sizeOfLastBlock { 0 };
    };

    // By ValueFlowSolver's numbers of what it has made.
    struct PartOfMadeNodes {
        uint32_t whole { 0 };
        uint32_t part { 0 };
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
        uint32_t endOfValuesShown { 0 };
        uint32_t endOfPartsWatched { 0 };
        uint32_t endOfNodesAskedWhetherEmpty { 0 };
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

    // The merge unites the uses of a built-in, so one that the log has already says nothing.
    void use(BuiltinID builtin, OptionSet<BuiltinUse> usesOfBuiltin)
    {
        if (builtin.index() >= m_usesLogged.size())
            m_usesLogged.grow(builtin.index() + 1);
        OptionSet<BuiltinUse>& logged = m_usesLogged[builtin.index()];
        if (logged.containsAll(usesOfBuiltin))
            return;
        logged.add(usesOfBuiltin);
        uses.append(Use { builtin, usesOfBuiltin });
    }

    void clear()
    {
        made.shrink(0);
        numberOfMadeNodes = 0;
        numberMadeWhereAskedFor = 0;
        m_madeByKey = HashedIndex();
        edges.shrink(0);
        reached.shrink(0);
        whoReached.shrink(0);
        uses.shrink(0);
        m_usesLogged.shrink(0);
        statementsWithMembersNotToldApart.shrink(0);
        runs.shrink(0);
        nodesRead.shrink(0);
        functionsAsked.shrink(0);
        nodesAskedFor.shrink(0);
        valuesShown.shrink(0);
        partsWatched.shrink(0);
        nodesAskedWhetherEmpty.shrink(0);
    }

    Vector<MadeNodes> made;
    uint32_t numberOfMadeNodes { 0 };
    // What this thread has made in the solver's own lists: see ValueFlowSolver::makeNodesWhereAskedFor(). The solver empties the three lists at the end of the turn. The parts of one whole stand in a row.
    uint32_t numberMadeWhereAskedFor { 0 };
    NumbersToGive recordsToGive;
    NumbersToGive nodesToGive;
    NumbersToGive nodesOfCallGroupsToGive;
    Vector<uint32_t> madeWhereAskedFor;
    Vector<uint32_t> madeWithStatementToRun;
    uint32_t numberOfRunsWhereMade { 0 };
    Vector<uint32_t> recordsOfRunsWhereMade;
    // For each entry of `reached`: the statement that reached the function, and what it counts as where a choice among statements shows.
    struct WhoReached {
        ValueFlowStatementID statement;
        uint64_t content { 0 };
    };
    Vector<WhoReached> whoReached;
    Vector<std::pair<ValueFlowStatementID, ValueSetID>> rememberedByStatementsRunWhereMade;
    Vector<PartOfMadeNodes> partsOfMadeNodes;
    Vector<ValueFlowEdge> edgesOfMadeNodes;
    Vector<ValueFlowEdge> edges;
    // An edge past this many is dropped: the solve is past its budget of edges, and the solver hears of it here.
    uint64_t mostEdges { UINT64_MAX };
    bool hasDroppedEdges { false };
    uint32_t numberOfConnectionsOfRun { 0 };
    Vector<FunctionID> reached;
    Vector<Use> uses;
    Vector<ValueFlowStatementID> statementsWithMembersNotToldApart;
    Vector<Run> runs;
    Vector<ValueFlowNodeID> nodesRead;
    Vector<FunctionID> functionsAsked;
    // The first node of what read(), written(), lookup(), group(), hub() and masked() have answered.
    Vector<ValueFlowNodeID> nodesAskedFor;
    // All that a node held when newSince() was asked about it. The merge keeps it by statement and node, and the next run of the statement is shown what the node holds beyond it.
    Vector<ValueFlowNodeSet> valuesShown;
    // What valueWithin() was asked: the statement runs again once the node holds more of the set.
    Vector<ValueFlowNodeSet> partsWatched;
    // What holdsAnything() has found empty: the statement runs again once the node holds something.
    Vector<ValueFlowNodeID> nodesAskedWhetherEmpty;

private:
    HashedIndex m_madeByKey;
    // By built-in.
    Vector<OptionSet<BuiltinUse>> m_usesLogged;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
