/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowGraph.h"

#if ENABLE(AOT)

#include <algorithm>
#include <optional>
#include <ranges>
#include <wtf/MonotonicTime.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t none = UINT32_MAX;

struct ValueFlowGraph::Worker {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Worker);

    explicit Worker(ValueSetMaker& maker)
        : maker(maker)
    {
    }

    ValueSetMaker& maker;
    Vector<uint32_t> movedNodes;
    Vector<uint32_t> dirtiedPieces;
    uint64_t evaluations { 0 };
    uint64_t work { 0 };
};

ValueFlowGraph::ValueFlowGraph(const ParallelLoops& loops, Makers makers)
    : m_loops(loops)
    , m_makers(makers)
{
    RELEASE_ASSERT(makers.size() >= loops.width());
    for (const std::unique_ptr<ValueSetMaker>& maker : makers)
        m_workers.append(makeUnique<Worker>(*maker));
    m_firstMember.append(0u);
}

ValueFlowGraph::~ValueFlowGraph() = default;

ValueFlowNodeID ValueFlowGraph::addNodes(uint32_t count)
{
    ValueFlowNodeID first(m_value.size());
    size_t size = m_value.size() + count;
    RELEASE_ASSERT(size < ValueFlowRoundLog::numberedByLog);
    m_value.insertFill(m_value.size(), ValueSets::nothing, count);
    m_mask.insertFill(m_mask.size(), ValueSets::anything, count);
    if (size > m_isAtRest.size())
        m_isAtRest.ensureSize(std::max(size, m_isAtRest.size() * 2));
    return first;
}

void ValueFlowGraph::setMask(ValueFlowNodeID node, ValueSetID mask, MaskKind kind)
{
    ASSERT(m_value[node.index()] == ValueSets::nothing);
    m_mask[node.index()] = mask;
    if (kind == MaskKind::Plain)
        m_hasPlainMask.set(node.index());
}

ValueSetID ValueFlowGraph::pass(ValueSetID value, uint32_t node, ValueSetMaker& maker) const
{
    ValueSetID mask = m_mask[node];
    if (mask == ValueSets::anything)
        return value;
    return m_hasPlainMask.get(node) ? maker.intersect(value, mask) : maker.pass(value, mask, m_standIn);
}

void ValueFlowGraph::makeConstant(ValueFlowNodeID node, ValueSetID set)
{
    m_value[node.index()] = set;
}

void ValueFlowGraph::startWithValueOf(ValueFlowNodeID node, ValueFlowNodeID other)
{
    m_value[node.index()] = pass(m_value[other.index()], node.index(), *m_makers[0]);
}

void ValueFlowGraph::putToRest(ValueFlowNodeID first, uint32_t count)
{
    for (uint32_t offset = 0; offset < count; ++offset)
        m_isAtRest.quickSet(first.index() + offset);
}

void ValueFlowGraph::wake(ValueFlowNodeID first, uint32_t count)
{
    for (uint32_t offset = 0; offset < count; ++offset) {
        m_isAtRest.quickClear(first.index() + offset);
        m_nodesWoken.append(first.index() + offset);
    }
}

bool ValueFlowGraph::EdgeSet::add(ValueFlowEdge edge)
{
    if ((m_size + 1) * 3 > m_slots.size() * 2)
        grow();
    uint64_t key = static_cast<uint64_t>(edge.from.index()) << 32 | edge.to.index();
    size_t mask = m_slots.size() - 1;
    size_t position = (key * 0x9e3779b97f4a7c15ull >> 24) & mask;
    for (; m_slots[position]; position = (position + 1) & mask) {
        if (m_slots[position] == key)
            return false;
    }
    m_slots[position] = key;
    ++m_size;
    return true;
}

bool ValueFlowGraph::EdgeSet::contains(ValueFlowEdge edge) const
{
    if (m_slots.isEmpty())
        return false;
    uint64_t key = static_cast<uint64_t>(edge.from.index()) << 32 | edge.to.index();
    size_t mask = m_slots.size() - 1;
    for (size_t position = (key * 0x9e3779b97f4a7c15ull >> 24) & mask; m_slots[position]; position = (position + 1) & mask) {
        if (m_slots[position] == key)
            return true;
    }
    return false;
}

void ValueFlowGraph::EdgeSet::grow()
{
    Vector<uint64_t> previous = std::exchange(m_slots, Vector<uint64_t>());
    m_slots.fill(0ull, std::max<size_t>(previous.size() * 2, 1024));
    size_t mask = m_slots.size() - 1;
    for (uint64_t key : previous) {
        if (!key)
            continue;
        size_t position = (key * 0x9e3779b97f4a7c15ull >> 24) & mask;
        while (m_slots[position])
            position = (position + 1) & mask;
        m_slots[position] = key;
    }
}

void ValueFlowGraph::addInitialEdges(std::span<const ValueFlowEdge> edges)
{
    for (ValueFlowEdge edge : edges) {
        if (edge.from != edge.to)
            m_edgesNotInRows.append(edge);
    }
}

size_t ValueFlowGraph::addEdges(std::span<const ValueFlowEdge> edges)
{
    size_t before = m_edgesNotInRows.size();
    for (ValueFlowEdge edge : edges) {
        if (edge.from != edge.to && m_edgeSet.add(edge))
            m_edgesNotInRows.append(edge);
    }
    return m_edgesNotInRows.size() - before;
}

auto ValueFlowGraph::buildRows(std::span<const ValueFlowEdge> edges, Direction direction, uint32_t numberOfNodes) -> Rows
{
    auto ownerOf = [&](ValueFlowEdge edge) {
        return direction == Direction::Predecessors ? edge.to.index() : edge.from.index();
    };
    auto otherOf = [&](ValueFlowEdge edge) {
        return direction == Direction::Predecessors ? edge.from.index() : edge.to.index();
    };

    Rows rows;
    rows.start.fill(0u, numberOfNodes + 1);
    for (ValueFlowEdge edge : edges)
        ++rows.start[ownerOf(edge) + 1];
    for (uint32_t node = 0; node < numberOfNodes; ++node)
        rows.start[node + 1] += rows.start[node];
    rows.others.grow(edges.size());
    Vector<uint32_t> next(rows.start.span().first(numberOfNodes));
    for (ValueFlowEdge edge : edges)
        rows.others[next[ownerOf(edge)]++] = otherOf(edge);
    return rows;
}

void ValueFlowGraph::RecentRows::add(std::span<const ValueFlowEdge> edges, Direction direction, uint32_t numberOfNodes)
{
    first.insertFill(first.size(), none, numberOfNodes - first.size());
    for (ValueFlowEdge edge : edges) {
        uint32_t owner = direction == Direction::Predecessors ? edge.to.index() : edge.from.index();
        uint32_t other = direction == Direction::Predecessors ? edge.from.index() : edge.to.index();
        entries.append(Entry { other, std::exchange(first[owner], static_cast<uint32_t>(entries.size())) });
    }
}

void ValueFlowGraph::RecentRows::clear()
{
    entries.shrink(0);
    first.shrink(0);
}

void ValueFlowGraph::foldRecentRowsIntoOlder()
{
    m_edgesInOlderRows.appendVector(m_edgesInRecentRows);
    m_edgesInRecentRows.clear();
    m_predecessors.recent.clear();
    m_successors.recent.clear();
    runTasksInParallel(m_loops, {
        [&] { m_predecessors.older = buildRows(m_edgesInOlderRows.span(), Direction::Predecessors, numberOfNodes()); },
        [&] { m_successors.older = buildRows(m_edgesInOlderRows.span(), Direction::Successors, numberOfNodes()); },
    });
}

void ValueFlowGraph::takeEdgesIntoRows()
{
    m_numberOfEdges += m_edgesNotInRows.size();
    if (m_edgesNotInRows.isEmpty())
        return;
    m_predecessors.recent.add(m_edgesNotInRows.span(), Direction::Predecessors, numberOfNodes());
    m_successors.recent.add(m_edgesNotInRows.span(), Direction::Successors, numberOfNodes());
    m_edgesInRecentRows.appendVector(m_edgesNotInRows);
    // Each edge is moved into the older rows a bounded number of times.
    if (m_edgesInRecentRows.size() * 8 > m_edgesInOlderRows.size())
        foldRecentRowsIntoOlder();
}

uint32_t ValueFlowGraph::levelAfterPredecessorsOf(uint32_t piece) const
{
    uint32_t level = 0;
    for (uint32_t member : membersOf(piece)) {
        forEachOther(m_predecessors, member, [&](uint32_t predecessor) {
            uint32_t other = m_piece[predecessor];
            if (other < piece)
                level = std::max(level, m_level[other] + 1);
        });
    }
    return level;
}

void ValueFlowGraph::makePiecesOfNewNodes()
{
    m_piece.insertFill(m_piece.size(), none, numberOfNodes() - m_piece.size());
    uint32_t firstNewPiece = m_level.size();
    for (uint32_t node = m_numberOfNodesWithPiece; node < numberOfNodes(); ++node) {
        m_piece[node] = m_level.size();
        m_members.append(node);
        m_firstMember.append(m_members.size());
        m_level.append(0u);
    }
    m_numberOfNodesWithPiece = numberOfNodes();
    for (uint32_t piece = firstNewPiece; piece < m_level.size(); ++piece)
        m_level[piece] = levelAfterPredecessorsOf(piece);
}

// Tarjan's algorithm over the rows of predecessors: a component comes out after every component that reaches it.
// `follows(node)` says whether the edges into the node count, `belongs(node)` which nodes are looked at at all.
namespace {

class Condensation {
public:
    explicit Condensation(uint32_t numberOfNodes)
    {
        m_index.fill(0u, numberOfNodes);
        m_low.fill(0u, numberOfNodes);
        component.fill(none, numberOfNodes);
    }

    void run(const auto& roots, NOESCAPE const Invocable<std::span<const uint32_t>(uint32_t)> auto& predecessorsOf, NOESCAPE const Invocable<bool(uint32_t)> auto& belongs, NOESCAPE const Invocable<void(std::span<const uint32_t>)> auto& found)
    {
        for (uint32_t root : roots) {
            if (m_index[root])
                continue;
            enter(root);
            while (!m_calls.isEmpty()) {
                Call& call = m_calls.last();
                uint32_t node = call.node;
                std::span<const uint32_t> predecessors = predecessorsOf(node);
                bool descended = false;
                while (call.next < predecessors.size()) {
                    uint32_t predecessor = predecessors[call.next++];
                    if (!belongs(predecessor))
                        continue;
                    if (!m_index[predecessor]) {
                        enter(predecessor);
                        descended = true;
                        break;
                    }
                    if (component[predecessor] == none)
                        m_low[node] = std::min(m_low[node], m_index[predecessor]);
                }
                if (descended)
                    continue;
                if (m_low[node] == m_index[node]) {
                    size_t first = m_stack.size();
                    do {
                        --first;
                        component[m_stack[first]] = numberOfComponents;
                    } while (m_stack[first] != node);
                    found(m_stack.span().subspan(first));
                    m_stack.shrink(first);
                    ++numberOfComponents;
                }
                m_calls.removeLast();
                if (!m_calls.isEmpty())
                    m_low[m_calls.last().node] = std::min(m_low[m_calls.last().node], m_low[node]);
            }
        }
    }

    Vector<uint32_t> component;
    uint32_t numberOfComponents { 0 };

private:
    struct Call {
        uint32_t node;
        uint32_t next;
    };

    void enter(uint32_t node)
    {
        m_index[node] = m_low[node] = ++m_counter;
        m_stack.append(node);
        m_calls.append(Call { node, 0 });
    }

    Vector<uint32_t> m_index;
    Vector<uint32_t> m_low;
    Vector<uint32_t> m_stack;
    Vector<Call> m_calls;
    uint32_t m_counter { 0 };
};

} // anonymous namespace

void ValueFlowGraph::order()
{
    if (!m_edgesInRecentRows.isEmpty())
        foldRecentRowsIntoOlder();

    uint32_t size = numberOfNodes();
    const Rows& rows = m_predecessors.older;
    m_members.shrink(0);
    m_members.reserveCapacity(size);
    m_firstMember.shrink(0);
    m_firstMember.append(0u);
    m_counters.nodesInCycles = 0;
    m_counters.largestCycle = 0;

    auto addPiece = [&](std::span<const uint32_t> members) {
        uint32_t piece = m_firstMember.size() - 1;
        bool holdTheSame = true;
        for (uint32_t member : members) {
            m_piece[member] = piece;
            holdTheSame &= m_value[member] == m_value[members[0]];
        }
        if (!holdTheSame)
            m_piecesOfUnequalMembers.append(piece);
        m_members.append(members);
        m_firstMember.append(m_members.size());
    };

    Condensation whole(size);
    std::optional<Condensation> withinCycles;
    whole.run(std::views::iota(0u, size), [&](uint32_t node) {
        return rows.of(node);
    }, [](uint32_t) {
        return true;
    }, [&](std::span<const uint32_t> members) {
        if (members.size() == 1) {
            addPiece(members);
            return;
        }
        m_counters.nodesInCycles += members.size();
        m_counters.largestCycle = std::max<uint32_t>(m_counters.largestCycle, members.size());
        bool hasMask = std::ranges::any_of(members, [&](uint32_t member) {
            return m_mask[member] != ValueSets::anything;
        });
        if (!hasMask) {
            addPiece(members);
            return;
        }
        // Cut at the nodes with a mask: without the edges into them only cycles of plain copies are left, whose members are equal. Only a cut edge can lead against the order that comes out.
        if (!withinCycles)
            withinCycles.emplace(size);
        uint32_t number = whole.component[members[0]];
        withinCycles->run(members, [&](uint32_t node) {
            return m_mask[node] == ValueSets::anything ? rows.of(node) : std::span<const uint32_t>();
        }, [&](uint32_t node) {
            return whole.component[node] == number;
        }, addPiece);
    });

    uint32_t numberOfPieces = m_firstMember.size() - 1;
    m_level.fill(0u, numberOfPieces);
    uint32_t highest = 0;
    for (uint32_t piece = 0; piece < numberOfPieces; ++piece) {
        m_level[piece] = levelAfterPredecessorsOf(piece);
        highest = std::max(highest, m_level[piece]);
    }

    m_hasEdgesAgainstTheOrder = false;
    m_workSinceOrdering = 0;
    ++m_counters.orderings;
    m_counters.numberOfLevels = highest + 1;
    m_counters.numberOfPieces = numberOfPieces;
}

void ValueFlowGraph::listAsDirty(uint32_t piece)
{
    uint32_t level = m_level[piece];
    if (level >= m_dirtyOfLevel.size())
        m_dirtyOfLevel.grow(level + 1);
    m_dirtyOfLevel[level].append(piece);
    ++m_numberOfDirtyPiecesListed;
}

// Whoever finds nothing pending at the piece lists it. A value that many predecessors hand on costs each of them one comparison.
void ValueFlowGraph::push(ValueSetID value, uint32_t piece, Worker& worker)
{
    std::atomic<uint32_t>& pending = m_pending[piece];
    uint32_t current = pending.load(std::memory_order_acquire);
    for (;;) {
        if (current == value.index())
            return;
        uint32_t united = worker.maker.unite(ValueSetID(current), value).index();
        if (united == current)
            return;
        if (pending.compare_exchange_weak(current, united, std::memory_order_acq_rel, std::memory_order_acquire)) {
            if (current == ValueSets::nothing.index())
                worker.dirtiedPieces.append(piece);
            return;
        }
    }
}

void ValueFlowGraph::pushToNode(ValueSetID value, uint32_t node, Worker& worker)
{
    if (value != ValueSets::nothing && !m_isAtRest.quickGet(node))
        push(value, m_piece[node], worker);
}

void ValueFlowGraph::evaluate(uint32_t piece, Worker& worker)
{
    ValueSetID arrived(m_pending[piece].exchange(ValueSets::nothing.index(), std::memory_order_acq_rel));
    std::span<const uint32_t> members = membersOf(piece);
    ++worker.evaluations;
    ++worker.work;

    arrived = pass(arrived, members[0], worker.maker);
    ValueSetID value = worker.maker.unite(m_value[members[0]], arrived);
    bool hasMoved = false;
    // The members of a cycle that has just been closed do not hold the same yet; what each of them holds is among what has arrived.
    for (uint32_t member : members) {
        if (m_value[member] == value)
            continue;
        m_value[member] = value;
        worker.movedNodes.append(member);
        hasMoved = true;
    }
    if (!hasMoved)
        return;
    for (uint32_t member : members) {
        forEachOther(m_successors, member, [&](uint32_t successor) {
            ++worker.work;
            if (m_isAtRest.quickGet(successor))
                return;
            uint32_t other = m_piece[successor];
            if (other != piece)
                push(value, other, worker);
        });
    }
}

void ValueFlowGraph::listPiecesDirtiedBy(Worker& worker)
{
    for (uint32_t piece : worker.dirtiedPieces)
        listAsDirty(piece);
    worker.dirtiedPieces.shrink(0);
}

void ValueFlowGraph::solve(Vector<ValueFlowNodeID>& moved)
{
    moved.shrink(0);
    MonotonicTime start = MonotonicTime::now();

    Vector<ValueFlowEdge> added = m_edgesNotInRows;
    takeEdgesIntoRows();
    m_edgesNotInRows.shrink(0);
    makePiecesOfNewNodes();
    for (ValueFlowEdge edge : added) {
        uint32_t from = m_piece[edge.from.index()];
        uint32_t to = m_piece[edge.to.index()];
        if (from != to && m_level[from] >= m_level[to]) {
            m_hasEdgesAgainstTheOrder = true;
            ++m_counters.edgesAgainstTheOrder;
        }
    }

    MonotonicTime afterRows = MonotonicTime::now();
    m_counters.rows += afterRows - start;

    // An ordering costs about one look at every node and edge. It is paid once the solves since the last one have cost as much.
    if (!m_counters.orderings || (m_hasEdgesAgainstTheOrder && m_workSinceOrdering > numberOfNodes() + m_numberOfEdges))
        order();

    MonotonicTime afterOrdering = MonotonicTime::now();
    m_counters.ordering += afterOrdering - afterRows;

    size_t numberOfPieces = m_level.size();
    if (numberOfPieces > m_capacityOfPending) {
        m_capacityOfPending = std::max(numberOfPieces, m_capacityOfPending * 2);
        m_pending = std::make_unique<std::atomic<uint32_t>[]>(m_capacityOfPending);
    }
    Worker& first = *m_workers[0];
    for (ValueFlowEdge edge : added)
        pushToNode(m_value[edge.from.index()], edge.to.index(), first);
    for (uint32_t node : m_nodesWoken) {
        forEachOther(m_predecessors, node, [&](uint32_t predecessor) {
            pushToNode(m_value[predecessor], node, first);
        });
    }
    m_nodesWoken.shrink(0);
    for (uint32_t piece : m_piecesOfUnequalMembers) {
        for (uint32_t member : membersOf(piece))
            push(m_value[member], piece, first);
    }
    m_piecesOfUnequalMembers.shrink(0);
    listPiecesDirtiedBy(first);

    constexpr size_t piecesPerShare = 64;
    constexpr size_t fewestPiecesForThreads = 1024;
    // A sweep goes up through the levels to the last. What an edge against the order makes dirty behind it waits for the next sweep, so that a node at which much comes together is evaluated once a sweep
    // and not once for each such edge.
    Vector<uint32_t> pieces;
    for (uint32_t level = 0; m_numberOfDirtyPiecesListed; ++level) {
        if (level >= m_dirtyOfLevel.size()) {
            level = 0;
            ++m_counters.sweeps;
        }
        if (m_dirtyOfLevel[level].isEmpty())
            continue;
        pieces.shrink(0);
        std::swap(pieces, m_dirtyOfLevel[level]);
        m_numberOfDirtyPiecesListed -= pieces.size();

        if (pieces.size() < fewestPiecesForThreads) {
            for (uint32_t piece : pieces)
                evaluate(piece, *m_workers[0]);
        } else {
            forEachIndexInParallel(m_loops, (pieces.size() + piecesPerShare - 1) / piecesPerShare, [&](unsigned share, unsigned worker) {
                for (uint32_t piece : pieces.subspan(share * piecesPerShare, std::min(piecesPerShare, pieces.size() - share * piecesPerShare)))
                    evaluate(piece, *m_workers[worker]);
            });
        }
        for (const std::unique_ptr<Worker>& worker : m_workers)
            listPiecesDirtiedBy(*worker);
    }

    for (const std::unique_ptr<Worker>& worker : m_workers) {
        for (uint32_t node : worker->movedNodes)
            moved.append(ValueFlowNodeID(node));
        worker->movedNodes.shrink(0);
        m_counters.evaluations += std::exchange(worker->evaluations, 0);
        m_counters.edgesLookedAt += worker->work;
        m_workSinceOrdering += std::exchange(worker->work, 0);
    }
    std::ranges::sort(moved);
    removeRepeatedElements(moved);
    m_counters.sweeping += MonotonicTime::now() - afterOrdering;
}

size_t ValueFlowGraph::memoryInBytes() const
{
    auto bytesOf = [](const auto& vector) {
        return vector.capacity() * sizeof(vector[0]);
    };
    size_t bytes = bytesOf(m_value) + bytesOf(m_mask) + m_edgeSet.memoryInBytes() + bytesOf(m_edgesInOlderRows) + bytesOf(m_edgesInRecentRows) + bytesOf(m_edgesNotInRows);
    for (const Adjacency* adjacency : { &m_predecessors, &m_successors })
        bytes += bytesOf(adjacency->older.start) + bytesOf(adjacency->older.others) + bytesOf(adjacency->recent.entries) + bytesOf(adjacency->recent.first);
    return bytes + bytesOf(m_piece) + bytesOf(m_firstMember) + bytesOf(m_members) + bytesOf(m_level) + m_capacityOfPending * sizeof(uint32_t);
}

} } } // namespace JSC::AOT::Typed

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

#endif // ENABLE(AOT)
