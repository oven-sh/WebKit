/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyInternals.h"

#include "AbstractSlotVisitorInlines.h"
#include "JSCInlines.h"
#include "SlotVisitorInlines.h"
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>

namespace JSC {
namespace Python {

// In what order finalizers run.
//
// With reference counts an object is finalized before what it alone refers to: the references it holds are dropped after its
// finalizer has run, and if the finalizer has found the object a new home, they are not dropped at all. Programs count on it: a
// __del__ that flushes self.file expects the file to be open.
//
// A collection finds all the unreachable objects at once. So it looks at how those with finalizers reach one another, through
// what else is unreachable, and ranks them. Objects that reach one another (a cycle) have the same rank, as they have no order
// in CPython either.

namespace {

// Walks what the collection has left unmarked, from the candidates, and marks nothing.
class UnreachableGraph final : public AbstractSlotVisitor {
public:
    UnreachableGraph(SlotVisitor& marker, std::span<JSCell* const> candidates)
        : AbstractSlotVisitor(marker.collector(), "PythonFinalizationOrder"_s, m_opaqueRootStorage)
        , m_marker(marker)
        , m_candidates(candidates)
        , m_count(candidates.size())
    {
    }

    Vector<unsigned> ranks();

    void append(const ConservativeRoots&) final { }
    void appendUnbarriered(JSCell* cell) final { reach(cell); }
    void appendHiddenUnbarriered(JSCell* cell) final { reach(cell); }
    bool isFirstVisit() const final { return false; }
    bool isMarked(const void* cell) const final { return m_marker.isMarked(cell); }
    bool isMarked(MarkedBlock& block, HeapCell* cell) const final { return m_marker.isMarked(block, cell); }
    bool isMarked(PreciseAllocation& allocation, HeapCell* cell) const final { return m_marker.isMarked(allocation, cell); }
    void markAuxiliary(const void*) final { }
    void reportExtraMemoryVisited(size_t) final { }
#if ENABLE(RESOURCE_USAGE)
    void reportExternalMemoryVisited(size_t) final { }
#endif
    bool mutatorsAreStopped() const final { return true; }
    void didRace(const VisitRaceKey&) final { }
    void visitAsConstraint(const JSCell* cell) final { visit(cell); }
    void addParallelConstraintTask(RefPtr<SharedTask<void(AbstractSlotVisitor&)>>) final { }
    void addParallelConstraintTask(RefPtr<SharedTask<void(SlotVisitor&)>>) final { }

private:
    // The nodes of the graph are the candidates, 0..n, and for each of them what was first reached from it, n..2n: its region.
    // What a candidate reaches it reaches through its region. If that runs into the region of another, it is taken to reach
    // all that region does, which may be more than it does reach: that costs precision, never more.
    unsigned regionOf(unsigned candidate) const { return m_count + candidate; }

    void visit(const JSCell* cell) { cell->methodTable()->visitChildren(const_cast<JSCell*>(cell), *this); }

    void reach(JSCell* cell)
    {
        if (!cell || m_marker.isMarked(cell))
            return;
        unsigned region = regionOf(m_current);
        auto result = m_nodes.add(cell, region);
        if (result.isNewEntry) {
            m_worklist.append(cell);
            return;
        }
        unsigned node = result.iterator->value;
        if (node != region && m_seenEdges.add(static_cast<uint64_t>(region) << 32 | node).isNewEntry)
            m_successors[region].append(node);
    }

    SlotVisitor& m_marker;
    std::span<JSCell* const> m_candidates;
    unsigned m_count;
    unsigned m_current { 0 };
    ConcurrentPtrHashSet m_opaqueRootStorage;
    UncheckedKeyHashMap<JSCell*, unsigned> m_nodes;
    UncheckedKeyHashSet<uint64_t, WTF::IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_seenEdges;
    Vector<Vector<unsigned, 2>> m_successors;
    Vector<JSCell*> m_worklist;
};

Vector<unsigned> UnreachableGraph::ranks()
{
    unsigned nodeCount = 2 * m_count;
    m_successors.grow(nodeCount);
    for (unsigned i = 0; i < m_count; ++i) {
        m_nodes.add(m_candidates[i], i);
        m_successors[i].append(regionOf(i));
    }
    for (m_current = 0; m_current < m_count; ++m_current) {
        visit(m_candidates[m_current]);
        while (!m_worklist.isEmpty())
            visit(m_worklist.takeLast());
    }

    // Strongly connected components (Tarjan, without recursion). A component is numbered once all it reaches is.
    constexpr unsigned none = std::numeric_limits<unsigned>::max();
    Vector<unsigned> index;
    index.fill(none, nodeCount);
    Vector<unsigned> low;
    low.fill(0, nodeCount);
    Vector<unsigned> component;
    component.fill(none, nodeCount);
    Vector<unsigned> stack;
    struct Frame {
        unsigned node;
        unsigned next;
    };
    Vector<Frame> frames;
    unsigned counter = 0;
    unsigned componentCount = 0;
    for (unsigned start = 0; start < nodeCount; ++start) {
        if (index[start] != none)
            continue;
        frames.append({ start, 0 });
        while (!frames.isEmpty()) {
            auto& frame = frames.last();
            unsigned node = frame.node;
            if (!frame.next) {
                index[node] = low[node] = counter++;
                stack.append(node);
            }
            if (frame.next < m_successors[node].size()) {
                unsigned successor = m_successors[node][frame.next++];
                if (index[successor] == none)
                    frames.append({ successor, 0 });
                else if (component[successor] == none)
                    low[node] = std::min(low[node], index[successor]);
                continue;
            }
            if (low[node] == index[node]) {
                unsigned member;
                do {
                    member = stack.takeLast();
                    component[member] = componentCount;
                } while (member != node);
                ++componentCount;
            }
            frames.removeLast();
            if (!frames.isEmpty())
                low[frames.last().node] = std::min(low[frames.last().node], low[node]);
        }
    }

    // The rank of a component: how many components with candidates in them lie on the longest way to it.
    Vector<bool> hasCandidate;
    hasCandidate.fill(false, componentCount);
    for (unsigned i = 0; i < m_count; ++i)
        hasCandidate[component[i]] = true;
    Vector<Vector<unsigned, 2>> members(componentCount);
    for (unsigned node = 0; node < nodeCount; ++node)
        members[component[node]].append(node);
    Vector<unsigned> rank;
    rank.fill(0, componentCount);
    for (unsigned c = componentCount; c--;) {
        unsigned next = rank[c] + hasCandidate[c];
        for (unsigned node : members[c]) {
            for (unsigned successor : m_successors[node]) {
                if (component[successor] != c)
                    rank[component[successor]] = std::max(rank[component[successor]], next);
            }
        }
    }
    return Vector<unsigned>(m_count, [&](size_t i) {
        return rank[component[i]];
    });
}

} // namespace

Vector<unsigned> finalizationRanks(SlotVisitor& marker, std::span<JSCell* const> candidates)
{
    if (candidates.size() < 2) {
        Vector<unsigned> ranks;
        ranks.fill(0, candidates.size());
        return ranks;
    }
    return UnreachableGraph(marker, candidates).ranks();
}

} // namespace Python
} // namespace JSC
