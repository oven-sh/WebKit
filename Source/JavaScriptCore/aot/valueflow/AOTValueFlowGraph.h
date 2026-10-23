/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTThreads.h"
#include "AOTValueFlowLog.h"
#include "AOTValueSets.h"
#include "AOTVector.h"
#include <array>
#include <atomic>
#include <bit>
#include <memory>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/MallocSpan.h>
#include <wtf/Noncopyable.h>
#include <wtf/RefPtr.h>
#include <wtf/Seconds.h>

namespace JSC { namespace AOT { namespace Typed {

// The nodes, the edges between them and the least solution of value(n) = what mask(n) lets through of the union of value(p) over the edges p -> n where a constant node holds its set for ever.
class ValueFlowGraph {
    WTF_MAKE_NONCOPYABLE(ValueFlowGraph);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ValueFlowGraph);
public:
    using Makers = std::span<const std::unique_ptr<ValueSetMaker>>;

    struct Counters {
        uint64_t evaluations { 0 };
        uint64_t edgesLookedAt { 0 };
        uint64_t waves { 0 };
        // A node counts once for each placeholder.
        uint32_t nodesFlowingIntoCoveredNodes { 0 };
        uint32_t nodesRetired { 0 };
        uint32_t edgesStraightIntoCoveredNodes { 0 };
        Seconds rows;
        Seconds waving;
    };

    // One maker for each thread of the loops.
    ValueFlowGraph(const ParallelLoops&, Makers);
    ~ValueFlowGraph();

    uint32_t numberOfNodes() const { return m_numberOfNodes; }
    size_t numberOfEdges() const { return m_numberOfEdges; }
    std::span<const ValueSetID> values() const LIFETIME_BOUND { return m_value.first(m_numberOfNodes); }
    ValueSetID valueOf(ValueFlowNodeID node) const { return m_value[node.index()]; }
    // Once nothing moves any more. Nothing follows from it.
    void setSolvedValue(ValueFlowNodeID node, ValueSetID value) { m_value[node.index()] = value; }
    ValueSetID maskOf(ValueFlowNodeID node) const { return m_masks[m_gates[node.index()].mask].admitted; }
    ValueFlowMaskKind maskKindOf(ValueFlowNodeID node) const { return m_masks[m_gates[node.index()].mask].kind; }
    // None where a placeholder passes the mask of the node as a member like any other.
    ValueSetID familyObjectsOf(ValueFlowNodeID node) const { return m_masks[m_gates[node.index()].mask].familyObjects; }
    void forEachSuccessor(ValueFlowNodeID node, NOESCAPE const Invocable<void(ValueFlowNodeID)> auto& functor) const
    {
        RELEASE_ASSERT(!m_hasReleasedSolvingState);
        forEachOther(m_successors[node.index()], [&](uint32_t successor) {
            functor(ValueFlowNodeID(successor));
        });
    }
    void forEachPredecessor(ValueFlowNodeID node, NOESCAPE const Invocable<void(ValueFlowNodeID)> auto& functor) const
    {
        if (m_hasReleasedSolvingState) {
            RELEASE_ASSERT(m_keepsPredecessors);
            for (uint32_t index = m_firstPredecessorInRows[node.index()]; index < m_firstPredecessorInRows[node.index() + 1]; ++index)
                functor(ValueFlowNodeID(m_predecessorsInRows[index]));
            return;
        }
        forEachOther(m_predecessors[node.index()], [&](uint32_t predecessor) {
            functor(ValueFlowNodeID(predecessor));
        });
    }

    ValueFlowNodeID addNodes(uint32_t count);
    // Before the node has a value. What the kinds and the family objects mean: ValueSetMaker::pass().
    void setMask(ValueFlowNodeID, ValueSetID, ValueFlowMaskKind = ValueFlowMaskKind::Plain, ValueSetID familyObjects = { });
    // Before any node has a value. Without them no member stands for another at a mask.
    void setPlaceholders(const ValueFlowPlaceholders& placeholders) { m_placeholders = placeholders; }
    // Before the first solve, once for a placeholder.
    void retireNodesCoveredBy(ValueFlowObjectID placeholder, ValueFlowNodeID covered, ValueSetID whole, ValueSetID coverable);
    // Before the first solve. A node of which the function says yes takes nothing of the set, early or late: that goes to the covered node. The function is asked from every thread of a solve.
    using IsMadeForPlaceholder = bool (*)(const void* context, ValueFlowNodeID);
    void keepOutOfNodesMadeForPlaceholders(ValueSetID keptOut, ValueFlowNodeID covered, IsMadeForPlaceholder, const void* context);
    // On account of any placeholder.
    bool isRetired(ValueFlowNodeID node) const { return node.index() < m_numberOfNodes && (m_gates[node.index()].flags & IsRetired); }
    // A placeholder with a covered node does not pass a mask that comes with family objects, and nothing takes its place by itself: what it covers moves.
    struct StoppedPlaceholder {
        ValueFlowNodeID node;
        ValueFlowObjectID placeholder;
    };
    void takeNodesAtWhichPlaceholdersStopped(Vector<StoppedPlaceholder>&);
    bool flowsIntoNodeCoveredBy(ValueFlowObjectID placeholder, ValueFlowNodeID) const;
    // Beside other threads: each with a worker's number of its own, and a node of its own.
    enum class EdgesOutOfConstant : bool { AreKeptAtTargetsAlone, AreListed };
    void makeConstant(ValueFlowNodeID, ValueSetID, unsigned worker = 0, EdgesOutOfConstant = EdgesOutOfConstant::AreKeptAtTargetsAlone);
    bool isConstant(ValueFlowNodeID node) const { return node.index() < m_numberOfNodes && isConstant(node.index()); }
    // In no order.
    void forEachListedEdgeOutOfConstants(NOESCAPE const Invocable<void(ValueFlowEdge)> auto& functor) const
    {
        RELEASE_ASSERT(!m_hasReleasedSolvingState);
        for (const std::unique_ptr<Shard>& shard : m_shards) {
            for (ValueFlowEdge edge : shard->listedEdgesOutOfConstants)
                functor(edge);
        }
    }
    // A node that has just been added holds from the start what its mask lets through of what the other node holds; that does not count as a move.
    void startWithValueOf(ValueFlowNodeID, ValueFlowNodeID other);
    // The same for many: `to` begins with what `from` holds. No `from` is the `to` of another.
    void startWithValuesOf(std::span<const ValueFlowEdge>);
    // A node at rest keeps what it holds and takes no more. One that is woken takes what it has missed in the next solve.
    void putToRest(ValueFlowNodeID first, uint32_t count);
    void wake(ValueFlowNodeID first, uint32_t count);
    // A retired node is not at rest: whoever connects something to it still has to, or what is written there never reaches the covered node.
    bool isAtRest(ValueFlowNodeID node) const { return m_gates[node.index()].flags & IsAtRest; }

    // The edges that are there from the start. They are not looked for when an edge is added later: an edge that is there twice does no harm.
    void addInitialEdges(std::span<const ValueFlowEdge>);
    // Leaves out the edges that have been added before. Answers how many had not. The edges may come in any order and with repeats.
    size_t addEdges(std::span<const ValueFlowEdge>);
    // Whether addEdges() has taken the edge, or the graph has made it itself. From any thread while no edge is added.
    bool hasAdded(ValueFlowEdge edge) const
    {
        RELEASE_ASSERT(!m_hasReleasedSolvingState);
        if (edge.from.index() >= m_numberOfNodes)
            return false;
        if (isConstant(edge.from.index())) {
            if (edge.to.index() >= m_numberOfNodes)
                return false;
            willUse(edge.to.index());
            const Links& predecessors = m_predecessors[edge.to.index()];
            if (predecessors.count > mostPredecessorsScanned)
                return m_shards[shardOf(edge.to.index())]->edgesOutOfConstantsIntoNodesWithManyPredecessors.contains(edge);
            const uint32_t* others = predecessors.begin();
            for (uint32_t index = 0; index < predecessors.count; ++index) {
                if (others[index] == edge.from.index())
                    return true;
            }
            return false;
        }
        willUse(edge.from.index());
        const Links& successors = m_successors[edge.from.index()];
        if (successors.count > mostSuccessorsScanned)
            return m_shards[shardOf(edge.from.index())]->edgesOfNodesWithManySuccessors.contains(edge);
        const uint32_t* others = successors.begin();
        for (uint32_t index = 0; index < successors.count; ++index) {
            if (others[index] == edge.to.index())
                return true;
        }
        return false;
    }

    ALWAYS_INLINE void prefetchLinksThatHold(ValueFlowEdge edge) const
    {
        if (edge.from.index() >= m_numberOfNodes || edge.to.index() >= m_numberOfNodes)
            return;
        __builtin_prefetch(isConstant(edge.from.index()) ? &m_predecessors[edge.to.index()] : &m_successors[edge.from.index()]);
    }
    // NeedsHasAdded: what hasAdded() will read has been prefetched.
    enum class EdgeLookup : uint8_t { Absent, Present, NeedsHasAdded };
    ALWAYS_INLINE EdgeLookup lookUpInInlineLinks(ValueFlowEdge edge) const
    {
        uint32_t from = edge.from.index();
        uint32_t to = edge.to.index();
        if (from >= m_numberOfNodes || to >= m_numberOfNodes)
            return EdgeLookup::Absent;
        bool isOutOfConstant = isConstant(from);
        uint32_t node = isOutOfConstant ? to : from;
        uint32_t other = isOutOfConstant ? from : to;
        willUse(node);
        const Links& links = isOutOfConstant ? m_predecessors[node] : m_successors[node];
        if (links.count <= 2)
            return (links.count && links.two[0] == other) || (links.count == 2 && links.two[1] == other) ? EdgeLookup::Present : EdgeLookup::Absent;
        static_assert(mostPredecessorsScanned == mostSuccessorsScanned);
        if (links.count <= mostSuccessorsScanned)
            __builtin_prefetch(links.block);
        else if (isOutOfConstant)
            m_shards[shardOf(node)]->edgesOutOfConstantsIntoNodesWithManyPredecessors.prefetch(edge);
        else
            m_shards[shardOf(node)]->edgesOfNodesWithManySuccessors.prefetch(edge);
        return EdgeLookup::NeedsHasAdded;
    }

    // Brings the values up to date with what has been added since the last call, and lists the nodes whose value has moved, in ascending order.
    enum class RetiredNodes : bool { AreFoundAndConnectedInSolve, AreFoundAndConnectedByCaller };
    // Behind solve(moved, AreFoundAndConnectedByCaller), beside whatever does not ask who is retired.
    void retireNodesAmong(std::span<const ValueFlowNodeID> moved);
    void solve(Vector<ValueFlowNodeID>& moved, RetiredNodes = RetiredNodes::AreFoundAndConnectedInSolve);
    void prefetchToStartWithValueOf(ValueFlowNodeID node, ValueFlowNodeID other) const
    {
        __builtin_prefetch(&m_rows[other.index()]);
        __builtin_prefetch(&m_value[other.index()]);
        __builtin_prefetch(&m_gates[node.index()], 1);
        __builtin_prefetch(&m_rows[node.index()], 1);
        __builtin_prefetch(&m_value[node.index()], 1);
    }
    bool hasNodesJustRetired() const;
    void connectPredecessorsOfNodesJustRetired();

    // Behind the last solve. Whoever is to ask for the predecessors of a node after that says so here.
    enum class Predecessors : bool { AreReleased, AreKept };
    void releaseSolvingState(Predecessors);
    void releaseRows();

    const Counters& counters() const { return m_counters; }
    // Behind releaseSolvingState(): what it was before.
    size_t memoryInBytes() const;

private:
    class Team;

    template<typename T>
    class ArrayByNode {
        WTF_MAKE_NONCOPYABLE(ArrayByNode);
    public:
        ArrayByNode() = default;
        ~ArrayByNode();

        void grow(size_t numberInUse, size_t capacity);
        void release();
        // The kernel takes as long to take pages back as to hand them out, and it takes them back from several threads at once.
        void release(Team&);
        T* data() const { return m_elements; }
        T& operator[](size_t index) LIFETIME_BOUND { return m_elements[index]; }
        const T& operator[](size_t index) const LIFETIME_BOUND { return m_elements[index]; }
        std::span<const T> first(size_t count) const LIFETIME_BOUND { return unsafeMakeSpan(m_elements, count); }
        size_t memoryInBytes() const { return m_capacity * sizeof(T); }

    private:
        T* m_elements { nullptr };
        size_t m_capacity { 0 };
    };

    enum Flag : uint8_t {
        IsAtRest = 1 << 0,
        IsRetired = 1 << 1,
        IgnoresArrivals = 1 << 2,
        IsCoveredNode = 1 << 3,
    };

    struct Gate {
        uint32_t mask : 24;
        uint32_t flags : 8;
    };

    struct Row {
        uint64_t low;
        uint32_t set;
        uint32_t firstArrivalPlusOne;
    };

    struct Links {
        const uint32_t* begin() const LIFETIME_BOUND { return count <= 2 ? two : block; }

        union {
            uint32_t two[2];
            uint32_t* block;
        };
        uint32_t count;
        uint32_t older;
    };

    class Arena {
        WTF_MAKE_NONCOPYABLE(Arena);
    public:
        Arena() = default;
        ~Arena();

        uint32_t* allocate(uint32_t numberOfWords);
        void free(uint32_t* block, uint32_t numberOfWords);
        void release();
        size_t memoryInBytes() const { return m_bytes; }

    private:
        std::array<uint32_t*, 32> m_free { };
        uint32_t* m_next { nullptr };
        uint32_t* m_end { nullptr };
        Vector<void*> m_slabs;
        size_t m_bytes { 0 };
    };

    class EdgeSet {
    public:
        bool add(ValueFlowEdge);
        bool contains(ValueFlowEdge) const;
        void prefetch(ValueFlowEdge) const;
        size_t memoryInBytes() const { return m_slots.sizeInBytes(); }

    private:
        void grow();

        MallocSpan<uint64_t> m_slots;
        size_t m_size { 0 };
    };

    struct Mask {
        ValueSetID admitted { ValueSets::anything };
        ValueSetID familyObjects;
        ValueFlowMaskKind kind { ValueFlowMaskKind::Plain };
        uint64_t lowMembersAdmitted { 0 };

        bool isSameAs(const Mask& other) const { return admitted == other.admitted && familyObjects == other.familyObjects && kind == other.kind; }
    };

    struct Covering {
        ValueFlowObjectID placeholder;
        uint32_t covered { 0 };
        uint64_t lowMembersOfWhole { 0 };
        ValueSetID listedMembersOfWhole;
        ValueSetID allButCoverable;
        uint64_t lowMembersNotCoverable { 0 };
        // What the covered node held when the wave began. Whoever would send it no more than that sends nothing.
        uint64_t lowMembersHeldByCoveredNode { 0 };
        uint32_t setHeldByCoveredNode { 0 };
        BitVector flowsIntoCovered;
        Vector<uint32_t> nodesJustRetired;
        BitVector isRetired;
        BitVector hasStoppedAt;
        Vector<uint32_t> nodesToLookAt;
    };

    struct Worker;

    static void forEachOther(const Links& links, NOESCAPE const Invocable<void(uint32_t)> auto& functor)
    {
        const uint32_t* others = links.begin();
        for (uint32_t index = 0; index < links.older; ++index)
            functor(others[index]);
        for (uint32_t index = links.count; index-- > links.older;)
            functor(others[index]);
    }

    static constexpr uint32_t numberOfLowMembers = 64;
    static constexpr uint32_t mostSuccessorsScanned = 8;
    static constexpr uint32_t mostPredecessorsScanned = 8;

    static constexpr unsigned numberOfShards = 64;
    static constexpr unsigned shardOf(uint32_t node) { return node >> 6 & (numberOfShards - 1); }
    struct Message {
        uint32_t to;
        uint32_t set;
        uint64_t low;
    };
    struct Arrival {
        uint64_t low;
        uint32_t set;
        uint32_t nextPlusOne;
    };
    struct Arrivals {
        Vector<uint32_t> nodes;
        Vector<Arrival> list;
    };
    struct alignas(128) Shard {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Shard);

        Arena blocksOfSuccessors;
        Arena blocksOfPredecessors;
        EdgeSet edgesOfNodesWithManySuccessors;
        EdgeSet edgesOutOfConstantsIntoNodesWithManyPredecessors;
        Vector<ValueFlowEdge> listedEdgesOutOfConstants;
        Arrivals arrivals;
        std::array<std::array<Vector<Message>, numberOfShards>, 2> messages;
        Vector<uint32_t> nodesWithRecentSuccessors;
        Vector<uint32_t> nodesWithRecentPredecessors;
        Vector<std::pair<uint32_t, uint32_t>> placeholdersStopped;
    };

    static void addLink(Arena&, Links&, uint32_t other);
    uint64_t lowMembersOf(ValueSetID, ValueSetMaker&) const;
    void pass(uint32_t node, uint32_t numberOfMask, uint64_t& low, uint64_t& lowInNoSet, ValueSetID& set, Worker&);
    void hold(uint32_t node, uint64_t low, ValueSetID value, Worker&);
    void takeArrival(Arrivals&, const Message&);
    void takeArrivals(Arrivals&, std::span<const Message>);
    void evaluate(uint32_t node, Arrivals&, Worker&, NOESCAPE const Invocable<void(const Message&)> auto& leave);
    void evaluateAll(Arrivals&, Worker&, NOESCAPE const Invocable<void(const Message&)> auto& leave);
    void runWave(Team&);
    void giveBackLargeBuffers();

    // In front of the first look at the rows or the links of a node. See makePagesWritable().
    ALWAYS_INLINE void willUse(uint32_t node) const
    {
        uint32_t page = node >> m_shiftFromNodeToPage;
        if (!(m_arePagesWritable[page / 64].load(std::memory_order_relaxed) >> page % 64 & 1)) [[unlikely]]
            makePagesWritable(page);
    }
    void makePagesWritable(uint32_t page) const;

    void takeEdgesIntoLists(Team&, std::span<const ValueFlowEdge>);
    bool addSuccessorIfAbsent(ValueFlowEdge);
    bool addEdgeOutOfConstantIfAbsent(ValueFlowEdge);
    bool addEdgeIfAbsent(ValueFlowEdge);
    void willHaveManyPredecessors(Shard&, uint32_t node);
    bool isConstant(uint32_t node) const { return m_isConstant[node / 64].load(std::memory_order_relaxed) >> node % 64 & 1; }
    // The shard whose thread adds the edge.
    unsigned shardThatKeeps(ValueFlowEdge edge) const { return shardOf(isConstant(edge.from.index()) ? edge.to.index() : edge.from.index()); }
    void countRecentEdgesAsOlder();
    void layPredecessorsInRows();

    bool passesEverythingOn(uint32_t node) const { return !m_gates[node].mask && !(m_gates[node].flags & IsAtRest); }
    bool takesNoMore(uint32_t node) const;
    void noteWhatCoveredNodesHold();
    bool didCoveredNodeHoldAllOf(uint32_t covered, uint64_t low, uint32_t set) const;
    ValueSetID whatRetiredNodeTakes(uint32_t node, ValueSetMaker&) const;
    bool retiredNodeTakesAnyOf(uint32_t node, uint64_t low, uint32_t set) const;
    void connectPredecessorsOfRetiredNodes(Team&);
    void connectStraightToCoveredNode(Covering&, uint32_t node);
    void markNodesFlowingIntoCoveredNodes(Team*, std::span<const ValueFlowEdge> added);
    void retireNodes(Team&, std::span<const ValueFlowNodeID> moved);
    void findIndices(Team*, size_t count, Vector<uint32_t>& found, NOESCAPE const Invocable<bool(size_t)> auto& matches);

    const ParallelLoops& m_loops;
    Makers m_makers;
    const ValueSets& m_sets;
    RefPtr<Team> m_team;
    ValueSetID m_lowMembers;

    uint32_t m_numberOfNodes { 0 };
    size_t m_capacity { 0 };
    ArrayByNode<Gate> m_gates;
    ArrayByNode<Row> m_rows;
    ArrayByNode<Links> m_successors;
    ArrayByNode<Links> m_predecessors;
    ArrayByNode<uint32_t> m_firstPredecessorInRows;
    ArrayByNode<uint32_t> m_predecessorsInRows;
    ArrayByNode<ValueSetID> m_value;
    // A bit for the nodes whose rows fill one page, and whose links fill one.
    mutable Vector<std::atomic<uint64_t>> m_arePagesWritable;
    // A bit a node.
    Vector<std::atomic<uint64_t>> m_isConstant;
    Vector<std::atomic<uint64_t>> m_areEdgesOutOfConstantListed;
    unsigned m_shiftFromNodeToPage { 0 };

    Vector<Mask> m_masks;
    HashedIndex m_numberOfMask;
    ValueFlowPlaceholders m_placeholders;
    Vector<uint32_t> m_nodesWoken;

    Vector<Covering, 2> m_coverings;
    IsMadeForPlaceholder m_isMadeForPlaceholder { nullptr };
    const void* m_contextOfIsMadeForPlaceholder { nullptr };
    ValueSetID m_allButKeptOutOfNodesMadeForPlaceholders { ValueSets::anything };
    uint64_t m_lowMembersKeptOutOfNodesMadeForPlaceholders { 0 };
    uint32_t m_coveredNodeThatTakesWhatIsKeptOut { 0 };
    BitVector m_hasMoved;

    Vector<uint8_t> m_isNewEdge;
    size_t m_numberOfEdges { 0 };
    size_t m_numberOfOlderEdges { 0 };
    size_t m_numberOfRecentEdges { 0 };
    Vector<ValueFlowEdge> m_edgesNotInLists;
    Vector<ValueFlowEdge> m_edgesOfSolve;
    Vector<Vector<uint32_t>> m_indicesBySource;
    Vector<Vector<ValueFlowEdge>> m_edgesByTarget;

    Vector<std::unique_ptr<Shard>> m_shards;
    std::array<Vector<Message>, 2> m_messages;
    Arrivals m_arrivals;
    std::atomic<size_t> m_numberOfMessagesWithShards { 0 };
    unsigned m_parity { 0 };
    bool m_hasLeftMessagesWithShards { false };
    bool m_hasSolved { false };
    bool m_hasReleasedSolvingState { false };
    bool m_keepsPredecessors { false };
    size_t m_memoryInBytesBeforeRelease { 0 };

    Vector<std::unique_ptr<Worker>> m_workers;
    Vector<Vector<uint32_t>> m_foundInShare;
    Vector<uint32_t> m_found;
    Counters m_counters;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
