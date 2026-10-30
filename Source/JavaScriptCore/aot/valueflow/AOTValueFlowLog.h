/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTTypeTable.h"
#include "AOTValueFlow.h"
#include "AOTVector.h"
#include <optional>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT { namespace Typed {

enum class ValueFlowMadeNodeKind : uint8_t {
    Constant,
    Read,
    Written,
    Lookup,
    CallGroup,
    Hub,
    Masked,
    Marked,
    WrittenUnderKind,
    ReadUnderKind,
    Instance,
};

enum class ValueFlowRareWrite : uint8_t {
    Accessor,
    Deletion,
    Copy,
    PrototypeLink,
};
constexpr unsigned numberOfValueFlowRareWrites = 4;

constexpr std::optional<ValueFlowRareWrite> rareWriteFoundUnder(ValueFlowNameKind kind)
{
    switch (shapeOf(kind).sort) {
    case ValueFlowNameSort::Getter:
    case ValueFlowNameSort::Setter:
        return ValueFlowRareWrite::Accessor;
    case ValueFlowNameSort::Deletion:
        return ValueFlowRareWrite::Deletion;
    case ValueFlowNameSort::Copy:
        return ValueFlowRareWrite::Copy;
    case ValueFlowNameSort::PrototypeLink:
        return ValueFlowRareWrite::PrototypeLink;
    case ValueFlowNameSort::Value:
    case ValueFlowNameSort::Redefinition:
    case ValueFlowNameSort::Other:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

struct ValueFlowNodeKey {
    ValueFlowNodeKey() = default;
    ValueFlowNodeKey(ValueFlowMadeNodeKind kind, uint8_t detail, uint32_t payload, ValueSetID set)
        : kind(kind)
        , detail(detail)
        , payload(payload)
        , set(set)
    {
    }

    static ValueFlowNodeKey constant(ValueSetID set) { return { ValueFlowMadeNodeKind::Constant, 0, 0, set }; }
    static ValueFlowNodeKey constant(ValueSetID set, ValueFlowUnknownSource source) { return { ValueFlowMadeNodeKind::Constant, static_cast<uint8_t>(static_cast<uint8_t>(source) + 1), 0, set }; }
    static ValueFlowNodeKey named(ValueFlowMadeNodeKind kind, ValueSetID set, ValueFlowName name) { return { kind, static_cast<uint8_t>(name.kind), name.payload, set }; }
    // What it answers is only called on the object that it was asked of.
    static ValueFlowNodeKey lookupForCall(ValueSetID set, ValueFlowName name) { return { ValueFlowMadeNodeKind::Lookup, static_cast<uint8_t>(static_cast<uint8_t>(name.kind) | isForCall), name.payload, set }; }
    // An edge stays in the graph when the statement that logged it asks for something else. So this read shares no node with the read of the same objects that a statement has connected whole.
    static ValueFlowNodeKey readOfWhatLetsInUnseenCode(ValueSetID set, ValueFlowName name) { return { ValueFlowMadeNodeKind::Read, static_cast<uint8_t>(static_cast<uint8_t>(name.kind) | isOfWhatLetsInUnseenCode), name.payload, set }; }
    static ValueFlowNodeKey underKind(ValueFlowMadeNodeKind kind, ValueSetID set, ValueFlowNameKind nameKind) { return { kind, static_cast<uint8_t>(nameKind), 0, set }; }
    static ValueFlowNodeKey callGroup(ValueSetID callees, ValueFlowCallKind kind, ValueFlowObjectID marker = { }) { return { ValueFlowMadeNodeKind::CallGroup, static_cast<uint8_t>(kind), marker ? marker.index() + 1 : 0, callees }; }
    static ValueFlowNodeKey instance(ValueFlowNodeID node, ValueSetID markerAlone) { return { ValueFlowMadeNodeKind::Instance, 0, node.index(), markerAlone }; }
    static ValueFlowNodeKey markersOf(FunctionID function) { return { ValueFlowMadeNodeKind::Instance, 1, function.index(), { } }; }
    static ValueFlowNodeKey restArgument(FunctionID function, unsigned index, ValueSetID markerAlone) { return { ValueFlowMadeNodeKind::Instance, static_cast<uint8_t>(2 + index), function.index(), markerAlone }; }
    static ValueFlowNodeKey hub(ValueFlowBehaviorID behavior) { return { ValueFlowMadeNodeKind::Hub, 0, behavior.index(), { } }; }
    static ValueFlowNodeKey masked(ValueFlowNodeID node, ValueSetID mask, ValueFlowMaskKind kind = ValueFlowMaskKind::Plain) { return { ValueFlowMadeNodeKind::Masked, static_cast<uint8_t>(kind), node.index(), mask }; }
    static ValueFlowNodeKey marked(ObjectMutation mutation) { return { ValueFlowMadeNodeKind::Marked, 0, static_cast<uint32_t>(mutation), { } }; }
    static ValueFlowNodeKey objectsThatAreNotPlain() { return { ValueFlowMadeNodeKind::Marked, 0, 1u << 16, { } }; }
    static ValueFlowNodeKey objectsWrittenUnderUnknownKey() { return { ValueFlowMadeNodeKind::Marked, 0, (1u << 16) + 1, { } }; }
    static ValueFlowNodeKey objectsWithRareWrite(ValueFlowRareWrite write) { return { ValueFlowMadeNodeKind::Marked, 0, (1u << 16) + 2 + static_cast<uint32_t>(write), { } }; }
    static ValueFlowNodeKey holdersOfLibraryExportsWrittenUnder(PropertyKeyID key) { return { ValueFlowMadeNodeKind::Marked, 1, key.index(), { } }; }

    ValueFlowName name() const { return { static_cast<ValueFlowNameKind>(detail & ~isForCall), payload }; }
    bool isLookupForCall() const { return kind == ValueFlowMadeNodeKind::Lookup && (detail & isForCall); }
    bool readsOnlyWhatLetsInUnseenCode() const { return kind == ValueFlowMadeNodeKind::Read && (detail & isOfWhatLetsInUnseenCode); }
    ValueFlowCallKind callKind() const { return static_cast<ValueFlowCallKind>(detail); }
    ValueFlowObjectID markerOfCallGroup() const { return payload ? ValueFlowObjectID(payload - 1) : ValueFlowObjectID(); }
    bool isInstanceOfNode() const { return kind == ValueFlowMadeNodeKind::Instance && !detail; }
    ValueFlowNodeID nodeOfInstance() const { return ValueFlowNodeID(payload); }
    ValueFlowBehaviorID behavior() const { return ValueFlowBehaviorID(payload); }
    ValueFlowNodeID maskedNode() const { return ValueFlowNodeID(payload); }
    ValueFlowMaskKind maskKind() const { return static_cast<ValueFlowMaskKind>(detail); }
    std::optional<ValueFlowUnknownSource> unknownSource() const { return detail ? std::optional(static_cast<ValueFlowUnknownSource>(detail - 1)) : std::nullopt; }

    unsigned numberOfNodes() const { return kind == ValueFlowMadeNodeKind::CallGroup ? numberOfNodesOfCallGroup : 1; }
    uint32_t hash() const
    {
        uint64_t hash = (static_cast<uint64_t>(payload) << 32 | set.index()) * 0x9e3779b97f4a7c15ull;
        hash = (hash ^ hash >> 32 ^ (static_cast<uint32_t>(kind) << 8 | detail)) * 0xbf58476d1ce4e5b9ull;
        return static_cast<uint32_t>(hash >> 32);
    }

    friend bool operator==(const ValueFlowNodeKey& a, const ValueFlowNodeKey& b) { return a.payload == b.payload && a.set == b.set && a.kind == b.kind && a.detail == b.detail; }

    static constexpr unsigned numberOfNodesOfCallGroup = 6 + numberOfGroupedArguments;
    static constexpr uint8_t isForCall = 0x80;
    static constexpr uint8_t isOfWhatLetsInUnseenCode = isForCall;

    ValueFlowMadeNodeKind kind { ValueFlowMadeNodeKind::Constant };
    uint8_t detail { 0 };
    // No part of the key. The solver's record of the key keeps its flags and its rank where there would be padding.
    uint8_t flagsOfRecord { 0 };
    uint8_t rankOfRecord { 0 };
    uint32_t payload { 0 };
    ValueSetID set;
};
static_assert(sizeof(ValueFlowNodeKey) == 12);

static_assert(static_cast<uint8_t>(ValueFlowNameKind::NamesOfMethods) < ValueFlowNodeKey::isForCall);

inline ValueFlowCallGroup callGroupAt(ValueFlowNodeID first)
{
    auto at = [&](unsigned offset) {
        return ValueFlowNodeID(first.index() + offset);
    };
    return { at(0), at(1), at(6), at(2), at(5), at(3), at(4) };
}

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

    struct NumbersToGive {
        uint32_t next { 0 };
        uint32_t end { 0 };
        uint32_t sizeOfLastBlock { 0 };
    };

    struct PartOfMadeNodes {
        uint32_t whole { 0 };
        uint32_t part { 0 };
    };

    struct Run {
        ValueFlowStatementID statement;
        uint32_t endOfNodesRead { 0 };
        uint32_t endOfFunctionsAsked { 0 };
        uint32_t endOfNodesAskedFor { 0 };
        uint32_t endOfEdges { 0 };
        uint32_t endOfReached { 0 };
        uint32_t numberOfConnections { 0 };
        uint32_t endOfMade { 0 };
        uint32_t endOfValuesShown { 0 };
        uint32_t endOfPartsWatched { 0 };
        uint32_t endOfNodesAskedWhetherEmpty { 0 };
        uint32_t endOfNamesWatched { 0 };
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
        auto shrinkToNothing = [](auto&... lists) {
            (lists.shrink(0), ...);
        };
        shrinkToNothing(made, edges, reached, whoReached, uses, m_usesLogged, statementsWithMembersNotToldApart, statementsThatAskProxiesByTheirTraps, libraryFunctionsLacked, runs, nodesRead, functionsAsked, madeNodesAskedFor, valuesShown, partsWatched, partsWatchedLast, nodesAskedWhetherEmpty, namesWatched);
        numberOfMadeNodes = 0;
        numberMadeWhereAskedFor = 0;
        m_madeByKey = HashedIndex();
    }

    Vector<MadeNodes> made;
    uint32_t numberOfMadeNodes { 0 };
    uint32_t numberMadeWhereAskedFor { 0 };
    NumbersToGive recordsToGive;
    NumbersToGive nodesToGive;
    NumbersToGive nodesOfCallGroupsToGive;
    Vector<uint32_t> madeWhereAskedFor;
    Vector<uint32_t> madeWithStatementToRun;
    Vector<uint32_t> askedForAtRest;
    uint32_t numberOfRunsWhereMade { 0 };
    Vector<uint32_t> recordsOfRunsWhereMade;
    struct WhoReached {
        ValueFlowStatementID statement;
        uint64_t content { 0 };
    };
    Vector<WhoReached> whoReached;
    Vector<std::pair<ValueFlowStatementID, ValueSetID>> rememberedByStatementsRunWhereMade;
    Vector<PartOfMadeNodes> partsOfMadeNodes;
    Vector<ValueFlowEdge> edgesOfMadeNodes;
    Vector<ValueFlowEdge> edges;
    uint32_t numberOfConnectionsOfRun { 0 };
    Vector<FunctionID> reached;
    Vector<Use> uses;
    Vector<ValueFlowStatementID> statementsWithMembersNotToldApart;
    Vector<ValueFlowStatementID> statementsThatAskProxiesByTheirTraps;
    Vector<RuntimeFunctionID> libraryFunctionsLacked;
    Vector<Run> runs;
    Vector<ValueFlowNodeID> nodesRead;
    Vector<FunctionID> functionsAsked;
    Vector<uint32_t> madeNodesAskedFor;
    Vector<ValueFlowNodeSet> valuesShown;
    Vector<ValueFlowNodeSet> partsWatched;
    Vector<uint32_t> partsWatchedLast;
    Vector<ValueFlowNodeID> nodesAskedWhetherEmpty;
    struct NameWatched {
        ValueFlowName name;
        ValueSetID within;
        uint32_t numberOfName { UINT32_MAX };
    };
    Vector<NameWatched> namesWatched;

private:
    HashedIndex m_madeByKey;
    Vector<OptionSet<BuiltinUse>> m_usesLogged;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
