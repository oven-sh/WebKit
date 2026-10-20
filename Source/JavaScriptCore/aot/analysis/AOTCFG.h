/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTEdgeResults.h"
#include "AOTIR.h"
#include <optional>
#include <span>
#include <wtf/BitVector.h>
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

const Target& targetOf(const Function&, Edge);
Target& targetOf(Function&, Edge);
ProgramPoint pointBeforeTerminatorOf(const Function&, BlockID);
uint32_t numberOfOutgoingEdges(const Block&);
Edge outgoingEdgeAt(const Function&, BlockID, uint32_t successorIndex);
uint32_t indexAfterCaughtValues(const Function&, const Block&);
bool returnsToCaller(TerminatorKind);

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
uint32_t numberOfExplicitArguments(const Function&, Edge);

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

class BlockIDDump {
public:
    explicit BlockIDDump(BlockID block)
        : m_block(block)
    {
    }

    void dump(PrintStream&) const;

private:
    BlockID m_block;
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

    BlockID root() const { return entryBlock; }
    unsigned numNodes() const { return m_numberOfBlocks; }
    unsigned index(BlockID block) const { return block.index(); }
    BlockID node(unsigned index) const { return m_isReachable.get(index) ? BlockID(index) : BlockID(); }
    template<typename T> Map<T> newMap() const { return Map<T>(m_numberOfBlocks); }

    std::span<const BlockID> successors(BlockID block) const { return entriesOf(block, m_successors, m_firstSuccessor); }
    std::span<const BlockID> predecessors(BlockID block) const { return entriesOf(block, m_predecessors, m_firstPredecessor); }
    std::span<const Edge> incomingEdges(BlockID block) const { return entriesOf(block, m_incomingEdges, m_firstIncomingEdge); }
    std::optional<Edge> singleIncomingEdge(BlockID) const;
    bool hasHandler(BlockID block) const { return m_hasHandler.get(block.index()); }
    bool hasHandlers() const { return !m_hasHandler.isEmpty(); }

    bool isReachable(BlockID block) const { return m_isReachable.get(block.index()); }
    std::span<const BlockID> reversePostOrder() const { return m_reversePostOrder.span(); }
    uint32_t reversePostOrderIndex(BlockID block) const
    {
        RELEASE_ASSERT(isReachable(block));
        return m_reversePostOrderIndex[block.index()];
    }

    BitVector blocksReachableFrom(BlockID start, const BitVector& barrier) const;
    bool isInCycleAvoiding(BlockID, BlockID avoidedBlock) const;

    BlockIDDump dump(BlockID block) const { return BlockIDDump(block); }
    void dump(PrintStream&) const;

private:
    void computeReversePostOrder(const Function&);
    void computeEdges(const Function&);
    void computePredecessors();

    template<typename T>
    std::span<const T> entriesOf(BlockID block, const Vector<T>& entries, const Vector<uint32_t>& firstEntry) const
    {
        RELEASE_ASSERT(block.index() < m_numberOfBlocks);
        uint32_t begin = firstEntry[block.index()];
        return entries.subspan(begin, firstEntry[block.index() + 1] - begin);
    }

    unsigned m_numberOfBlocks { 0 };
    BitVector m_isReachable;
    BitVector m_hasHandler;
    Vector<BlockID> m_reversePostOrder;
    Vector<uint32_t> m_reversePostOrderIndex;
    Vector<BlockID> m_successors;
    Vector<uint32_t> m_firstSuccessor;
    Vector<Edge> m_incomingEdges;
    Vector<uint32_t> m_firstIncomingEdge;
    Vector<BlockID> m_predecessors;
    Vector<uint32_t> m_firstPredecessor;
};

uint32_t numberOfSharedExplicitParameters(const Function&, const CFG&, BlockID);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
