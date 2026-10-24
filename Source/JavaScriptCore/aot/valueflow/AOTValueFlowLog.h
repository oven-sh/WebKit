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
    case ValueFlowNameKind::Property:
    case ValueFlowNameKind::Elements:
    case ValueFlowNameKind::UnknownKey:
    case ValueFlowNameKind::EveryProperty:
    case ValueFlowNameKind::RedefinedMember:
    case ValueFlowNameKind::RedefinedMemberUnderUnknownKey:
    case ValueFlowNameKind::PrivateField:
    case ValueFlowNameKind::Slot:
    case ValueFlowNameKind::HiddenSlot:
    case ValueFlowNameKind::InternalFields:
    case ValueFlowNameKind::SteppedValues:
    case ValueFlowNameKind::ReturnedValue:
    case ValueFlowNameKind::SentValues:
    case ValueFlowNameKind::OpenedBy:
    case ValueFlowNameKind::ResolvedPromises:
    case ValueFlowNameKind::RejectedPromises:
    case ValueFlowNameKind::AllocatedAs:
    case ValueFlowNameKind::OpenedIterators:
    case ValueFlowNameKind::IteratedValues:
    case ValueFlowNameKind::IteratorResults:
    case ValueFlowNameKind::AwaitedValue:
    case ValueFlowNameKind::MemberWritten:
    case ValueFlowNameKind::EveryMemberWritten:
    case ValueFlowNameKind::EveryMemberRead:
    case ValueFlowNameKind::RestArgument:
    case ValueFlowNameKind::AddedNames:
    case ValueFlowNameKind::FoundThroughEscapedInstances:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

struct ValueFlowNodeKey {
    static ValueFlowNodeKey constant(ValueSetID set) { return { ValueFlowMadeNodeKind::Constant, 0, 0, set }; }
    static ValueFlowNodeKey constant(ValueSetID set, ValueFlowUnknownSource source) { return { ValueFlowMadeNodeKind::Constant, static_cast<uint8_t>(static_cast<uint8_t>(source) + 1), 0, set }; }
    static ValueFlowNodeKey named(ValueFlowMadeNodeKind kind, ValueSetID set, ValueFlowName name) { return { kind, static_cast<uint8_t>(name.kind), name.payload, set }; }
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

    ValueFlowName name() const { return { static_cast<ValueFlowNameKind>(detail), payload }; }
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
        madeNodesAskedFor.shrink(0);
        valuesShown.shrink(0);
        partsWatched.shrink(0);
        partsWatchedLast.shrink(0);
        nodesAskedWhetherEmpty.shrink(0);
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
    uint64_t mostEdges { UINT64_MAX };
    bool hasDroppedEdges { false };
    uint32_t numberOfConnectionsOfRun { 0 };
    Vector<FunctionID> reached;
    Vector<Use> uses;
    Vector<ValueFlowStatementID> statementsWithMembersNotToldApart;
    Vector<Run> runs;
    Vector<ValueFlowNodeID> nodesRead;
    Vector<FunctionID> functionsAsked;
    Vector<uint32_t> madeNodesAskedFor;
    Vector<ValueFlowNodeSet> valuesShown;
    Vector<ValueFlowNodeSet> partsWatched;
    Vector<uint32_t> partsWatchedLast;
    Vector<ValueFlowNodeID> nodesAskedWhetherEmpty;

private:
    HashedIndex m_madeByKey;
    Vector<OptionSet<BuiltinUse>> m_usesLogged;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
