/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTThreads.h"
#include "AOTValueFlowLog.h"
#include "AOTValueSets.h"
#include <atomic>
#include <memory>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/Seconds.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

// The nodes, the edges between them and the least solution of
//     value(n) = mask(n) & union of value(p) over the edges p -> n
// where a constant node holds its set for ever. Edges and nodes are only ever added, so values only ever grow, and after an addition only what the addition reaches is computed again.
class ValueFlowGraph {
    WTF_MAKE_NONCOPYABLE(ValueFlowGraph);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ValueFlowGraph);
public:
    using Makers = std::span<const std::unique_ptr<ValueSetMaker>>;

    struct Counters {
        uint64_t evaluations { 0 };
        uint64_t edgesLookedAt { 0 };
        uint64_t sweeps { 0 };
        uint64_t orderings { 0 };
        uint64_t edgesAgainstTheOrder { 0 };
        uint32_t numberOfLevels { 0 };
        uint32_t numberOfPieces { 0 };
        uint32_t nodesInCycles { 0 };
        uint32_t largestCycle { 0 };
        Seconds rows;
        Seconds ordering;
        Seconds sweeping;
    };

    // One maker for each thread of the loops.
    ValueFlowGraph(const ParallelLoops&, Makers);
    ~ValueFlowGraph();

    uint32_t numberOfNodes() const { return m_value.size(); }
    size_t numberOfEdges() const { return m_numberOfEdges; }
    std::span<const ValueSetID> values() const LIFETIME_BOUND { return m_value.span(); }
    ValueSetID valueOf(ValueFlowNodeID node) const { return m_value[node.index()]; }
    // Once nothing moves any more. Nothing follows from it.
    void setSolvedValue(ValueFlowNodeID node, ValueSetID value) { m_value[node.index()] = value; }
    ValueSetID maskOf(ValueFlowNodeID node) const { return m_mask[node.index()]; }
    void forEachPredecessor(ValueFlowNodeID node, NOESCAPE const Invocable<void(ValueFlowNodeID)> auto& functor) const
    {
        forEachOther(m_predecessors, node.index(), [&](uint32_t predecessor) {
            functor(ValueFlowNodeID(predecessor));
        });
    }

    ValueFlowNodeID addNodes(uint32_t count);
    // Before the node has a value.
    enum class MaskKind : uint8_t {
        // The stand-in passes as what it may stand for.
        OfType,
        // What passes is what is in both.
        Plain,
    };
    void setMask(ValueFlowNodeID, ValueSetID, MaskKind = MaskKind::OfType);
    // Before any node has a value. Without it a mask lets through what it admits and nothing else.
    void setStandIn(const ValueFlowStandIn& standIn) { m_standIn = standIn; }
    void makeConstant(ValueFlowNodeID, ValueSetID);
    // A node that has just been added holds from the start what its mask lets through of what the other node holds; that does not count as a move.
    void startWithValueOf(ValueFlowNodeID, ValueFlowNodeID other);
    // A node at rest keeps what it holds and takes no more. One that is woken takes what it has missed in the next solve.
    void putToRest(ValueFlowNodeID first, uint32_t count);
    void wake(ValueFlowNodeID first, uint32_t count);
    bool isAtRest(ValueFlowNodeID node) const { return m_isAtRest.quickGet(node.index()); }

    // The edges that are there from the start. They are not looked for when an edge is added later: an edge that is there twice does no harm.
    void addInitialEdges(std::span<const ValueFlowEdge>);
    // Leaves out the edges that have been added before. Answers how many had not. The edges may come in any order and with repeats.
    size_t addEdges(std::span<const ValueFlowEdge>);
    // Whether addEdges() has taken the edge. From any thread while no edge is added.
    bool hasAdded(ValueFlowEdge edge) const { return m_edgeSet.contains(edge); }

    // Brings the values up to date with what has been added since the last call, and lists the nodes whose value has moved, in ascending order.
    void solve(Vector<ValueFlowNodeID>& moved);

    const Counters& counters() const { return m_counters; }
    size_t memoryInBytes() const;

private:
    // For each node the nodes at the other end of its edges of one direction, for the edges that were there when the rows were built.
    struct Rows {
        std::span<const uint32_t> of(uint32_t node) const LIFETIME_BOUND
        {
            if (node + 1 >= start.size())
                return { };
            return others.subspan(start[node], start[node + 1] - start[node]);
        }

        Vector<uint32_t> start;
        Vector<uint32_t> others;
    };

    enum class Direction : uint8_t { Predecessors, Successors };

    // The same for the few edges that have come since: a chain per node. Adding an edge costs the same however many there are.
    struct RecentRows {
        struct Entry {
            uint32_t other;
            uint32_t next;
        };

        void add(std::span<const ValueFlowEdge>, Direction, uint32_t numberOfNodes);
        void clear();

        // By node; shorter than the nodes are many where the last ones have no entry.
        Vector<uint32_t> first;
        Vector<Entry> entries;
    };

    struct Adjacency {
        Rows older;
        RecentRows recent;
    };

    class EdgeSet {
    public:
        bool add(ValueFlowEdge);
        bool contains(ValueFlowEdge) const;
        size_t memoryInBytes() const { return m_slots.capacity() * sizeof(uint64_t); }

    private:
        void grow();

        // Zero for a free slot: no edge leads from node 0 to node 0.
        Vector<uint64_t> m_slots;
        size_t m_size { 0 };
    };

    struct Worker;

    static Rows buildRows(std::span<const ValueFlowEdge>, Direction, uint32_t numberOfNodes);
    void takeEdgesIntoRows();
    void foldRecentRowsIntoOlder();

    static void forEachOther(const Adjacency& adjacency, uint32_t node, NOESCAPE const Invocable<void(uint32_t)> auto& functor)
    {
        for (uint32_t other : adjacency.older.of(node))
            functor(other);
        if (node >= adjacency.recent.first.size())
            return;
        for (uint32_t entry = adjacency.recent.first[node]; entry != UINT32_MAX; entry = adjacency.recent.entries[entry].next)
            functor(adjacency.recent.entries[entry].other);
    }

    std::span<const uint32_t> membersOf(uint32_t piece) const LIFETIME_BOUND { return m_members.subspan(m_firstMember[piece], m_firstMember[piece + 1] - m_firstMember[piece]); }

    void order();
    void makePiecesOfNewNodes();
    uint32_t levelAfterPredecessorsOf(uint32_t piece) const;
    ValueSetID pass(ValueSetID, uint32_t node, ValueSetMaker&) const;
    void listAsDirty(uint32_t piece);
    void push(ValueSetID, uint32_t piece, Worker&);
    void pushToNode(ValueSetID, uint32_t node, Worker&);
    void evaluate(uint32_t piece, Worker&);
    void listPiecesDirtiedBy(Worker&);

    const ParallelLoops& m_loops;
    Makers m_makers;

    Vector<ValueSetID> m_value;
    Vector<ValueSetID> m_mask;
    ValueFlowStandIn m_standIn;
    BitVector m_hasPlainMask;
    BitVector m_isAtRest;
    Vector<uint32_t> m_nodesWoken;

    EdgeSet m_edgeSet;
    size_t m_numberOfEdges { 0 };
    Vector<ValueFlowEdge> m_edgesInOlderRows;
    Vector<ValueFlowEdge> m_edgesInRecentRows;
    Vector<ValueFlowEdge> m_edgesNotInRows;
    Adjacency m_predecessors;
    Adjacency m_successors;

    // A piece is what is evaluated at once: a node, or the nodes of a cycle of edges into nodes without a mask, which all hold the same. Pieces are numbered so that, but for the edges against the order,
    // a piece comes after the pieces it takes from; pieces of one level take nothing from each other.
    Vector<uint32_t> m_piece;
    Vector<uint32_t> m_firstMember;
    Vector<uint32_t> m_members;
    Vector<uint32_t> m_level;
    uint32_t m_numberOfNodesWithPiece { 0 };
    bool m_hasEdgesAgainstTheOrder { false };
    // In nodes and edges looked at.
    uint64_t m_workSinceOrdering { 0 };

    // What has arrived at a piece and has not been taken into its value. A piece with something here is listed as dirty.
    std::unique_ptr<std::atomic<uint32_t>[]> m_pending;
    size_t m_capacityOfPending { 0 };
    Vector<uint32_t> m_piecesOfUnequalMembers;
    Vector<Vector<uint32_t>> m_dirtyOfLevel;
    size_t m_numberOfDirtyPiecesListed { 0 };

    Vector<std::unique_ptr<Worker>> m_workers;
    Counters m_counters;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
