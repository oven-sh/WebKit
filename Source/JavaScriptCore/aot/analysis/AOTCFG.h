/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTEdgeResults.h"
#include "AOTIR.h"
#include "AOTVector.h"
#include <optional>
#include <span>
#include "AOTBitVector.h"
#include <wtf/IndexMap.h>
#include <wtf/IndexSet.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

inline constexpr BlockID entryBlock { 0u };

enum class EdgeKind : uint8_t { Normal, Handler };

struct Edge {
    BlockID from;
    uint32_t targetIndex { 0 };
    EdgeKind kind { EdgeKind::Normal };

    static constexpr Edge toHandlerOf(BlockID block) { return { block, 0, EdgeKind::Handler }; }

    constexpr bool isHandlerEdge() const { return kind == EdgeKind::Handler; }

    friend constexpr bool operator==(const Edge&, const Edge&) = default;
};

struct ProgramPoint {
    BlockID block;
    uint32_t index { 0 };

    static constexpr ProgramPoint atHeadOf(BlockID block) { return { block, 0 }; }

    friend constexpr bool operator==(const ProgramPoint&, const ProgramPoint&) = default;
};

template<typename BlockType>
ALWAYS_INLINE auto& findTarget(BlockType& block, Edge edge)
{
    if (!edge.isHandlerEdge()) [[likely]]
        return block.terminator.targets[edge.targetIndex];
    RELEASE_ASSERT(block.handler);
    return *block.handler;
}

inline const Target& targetOf(const Function& function, Edge edge) { return findTarget(function.at(edge.from), edge); }
inline Target& editTarget(Function& function, Edge edge, OptionSet<BlockEdit> edits = { BlockEdit::Edges, BlockEdit::Arguments }) { return findTarget(function.edit(edge.from, edits), edge); }
inline ProgramPoint pointBeforeTerminatorOf(const Function& function, BlockID block) { return { block, static_cast<uint32_t>(function.at(block).values.size()) }; }
inline uint32_t numberOfOutgoingEdges(const Block& block) { return static_cast<uint32_t>(block.terminator.targets.size()) + (block.handler ? 1 : 0); }
Edge outgoingEdgeAt(const Function&, BlockID, uint32_t successorIndex);
uint32_t indexAfterCaughtValues(const Function&, const Block&);

enum class WayOut : uint8_t { ToBlocks, ToCaller, Abnormal };

inline WayOut wayOutOf(TerminatorKind kind)
{
    switch (kind) {
    case TerminatorKind::Return:
    case TerminatorKind::ReturnCall:
        return WayOut::ToCaller;
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
    case TerminatorKind::Unreachable:
        return WayOut::Abnormal;
    case TerminatorKind::Jump:
    case TerminatorKind::Branch:
    case TerminatorKind::Switch:
    case TerminatorKind::SwitchString:
    case TerminatorKind::SwitchOnShape:
    case TerminatorKind::TryCallInline:
    case TerminatorKind::TestAndBranch:
        return WayOut::ToBlocks;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

inline bool returnsToCaller(TerminatorKind kind) { return wayOutOf(kind) == WayOut::ToCaller; }

void forEachOutgoingEdge(const Function& function, BlockID from, NOESCAPE const Invocable<void(Edge, const Target&)> auto& functor)
{
    const Block& block = function.at(from);
    for (uint32_t targetIndex = 0; targetIndex < block.terminator.targets.size(); ++targetIndex)
        functor(Edge { from, targetIndex }, block.terminator.targets[targetIndex]);
    if (block.handler)
        functor(Edge::toHandlerOf(from), *block.handler);
}

struct IncomingValue {
    enum class Kind : uint8_t { Argument, EdgeResult };

    Kind kind { Kind::Argument };
    ValueID argument;
    EdgeResultPosition result;
};

IncomingValue incomingValueOf(const Function&, Edge, uint32_t parameterIndex);
ValueID valuePassedOn(const Function&, Edge, uint32_t parameterIndex);
inline uint32_t numberOfExplicitArguments(const Function& function, Edge edge) { return static_cast<uint32_t>(targetOf(function, edge).arguments.size()); }

void forEachEdgeResult(const Function& function, BlockID from, NOESCAPE const Invocable<void(const EdgeResult&, ValueID parameter)> auto& functor)
{
    auto& targets = function.at(from).terminator.targets;
    for (uint32_t targetIndex = 0; targetIndex < targets.size(); ++targetIndex) {
        auto& parameters = function.at(targets[targetIndex].block).parameters;
        uint32_t firstParameter = firstEdgeResultParameter(function, from, targetIndex);
        uint32_t numberOfResults = numberOfEdgeResults(function, from, targetIndex);
        for (uint32_t resultIndex = 0; resultIndex < numberOfResults; ++resultIndex) {
            std::optional<EdgeResult> result = tryGetEdgeResult(function, { from, targetIndex, resultIndex });
            if (!result || firstParameter + resultIndex >= parameters.size())
                continue;
            functor(*result, parameters[firstParameter + resultIndex]);
        }
    }
}

struct BlockIDDump {
    void dump(PrintStream&) const;

    BlockID block;
};

class CFG {
    WTF_MAKE_NONCOPYABLE(CFG);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(CFG);
public:
    using Node = BlockID;
    using Set = IndexSet<BlockID>;
    template<typename T> using Map = IndexMap<BlockID, T>;
    using List = Vector<BlockID, 4>;

    explicit CFG(const Function&);
    NEVER_INLINE ~CFG() = default;

    bool describes(const Function&) const;

    BlockID root() const { return entryBlock; }
    unsigned numNodes() const { return m_numberOfBlocks; }
    unsigned index(BlockID block) const { return block.index(); }
    BlockID node(unsigned index) const { return isReachable(BlockID(index)) ? BlockID(index) : BlockID(); }
    template<typename T> Map<T> newMap() const { return Map<T>(m_numberOfBlocks); }

    ALWAYS_INLINE std::span<const BlockID> successors(BlockID block) const
    {
        RELEASE_ASSERT(block.index() < m_numberOfBlocks);
        const uint32_t* first = m_firstSuccessor + block.index();
        return { m_successors + first[0], first[1] - first[0] };
    }
    std::span<const BlockID> successorsOfAllBlocks() const { return { m_successors, m_firstSuccessor[m_numberOfBlocks] }; }
    std::span<const uint32_t> firstSuccessorOfBlocks() const { return { m_firstSuccessor, static_cast<size_t>(m_numberOfBlocks) + 1 }; }
    std::span<const BlockID> predecessors(BlockID block) const
    {
        if (m_firstPredecessor.isEmpty())
            computePredecessors();
        RELEASE_ASSERT(block.index() < m_numberOfBlocks);
        std::span<const uint32_t> first = m_firstPredecessor.span();
        uint32_t begin = first[block.index()];
        return m_predecessors.span().subspan(begin, first[block.index() + 1] - begin);
    }
    ALWAYS_INLINE std::span<const Edge> incomingEdges(BlockID block) const
    {
        if (!m_firstIncomingEdge) [[unlikely]]
            computeIncomingEdges();
        RELEASE_ASSERT(block.index() < m_numberOfBlocks);
        const uint32_t* first = m_firstIncomingEdge + block.index();
        return { m_incomingEdges + first[0], first[1] - first[0] };
    }
    std::optional<Edge> singleIncomingEdge(BlockID) const;
    bool hasHandler(BlockID block) const { return m_hasHandler.get(block.index()); }
    bool hasHandlers() const { return !m_hasHandler.isEmpty(); }
    bool hasCycle() const { return m_hasCycle; }
    uint32_t largestNumberOfSuccessors() const
    {
        if (m_largestNumberOfSuccessors == notReached) [[unlikely]]
            findLargestNumberOfSuccessors();
        return m_largestNumberOfSuccessors;
    }

    bool isReachable(BlockID block) const { return block.index() < m_numberOfBlocks && m_reversePostOrderIndex[block.index()] != notReached; }
    std::span<const BlockID> reversePostOrder() const { return m_reversePostOrder; }
    uint32_t reversePostOrderIndex(BlockID block) const
    {
        RELEASE_ASSERT(isReachable(block));
        return m_reversePostOrderIndex[block.index()];
    }

    BitVector blocksReachableFrom(BlockID start, const BitVector& barrier) const;
    // Whether blocksReachableFrom() holds one of them. It goes no further than the first.
    bool reachesAnyOf(BlockID start, const BitVector& barrier, const BitVector& blocks) const;
    bool isInCycleAvoiding(BlockID, BlockID avoidedBlock) const;

    BlockIDDump dump(BlockID block) const { return { block }; }
    void dump(PrintStream&) const;

private:
    static constexpr uint32_t notReached = UINT32_MAX;
    static constexpr uint32_t beingWalked = UINT32_MAX - 1;

    void forgetUnreachableBlocks(uint32_t numberOfUnreachableBlocks);
    void computeIncomingEdges() const;
    void computePredecessors() const;
    void findLargestNumberOfSuccessors() const;

    unsigned m_numberOfBlocks { 0 };
    bool m_hasCycle { false };
    mutable uint32_t m_largestNumberOfSuccessors { notReached };
    BitVector m_hasHandler;
    Vector<uint32_t, 88> m_tables;
    const uint32_t* m_firstSuccessor { nullptr };
    const BlockID* m_successors { nullptr };
    const uint32_t* m_reversePostOrderIndex { nullptr };
    std::span<const BlockID> m_reversePostOrder;
    mutable Vector<uint32_t, 72> m_incomingTables;
    mutable const uint32_t* m_firstIncomingEdge { nullptr };
    mutable const Edge* m_incomingEdges { nullptr };
    mutable Vector<BlockID> m_predecessors;
    mutable Vector<uint32_t> m_firstPredecessor;
};

uint32_t numberOfSharedExplicitParameters(const Function&, const CFG&, BlockID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
