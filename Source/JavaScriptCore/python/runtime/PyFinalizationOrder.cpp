/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyInternals.h"

#include "AbstractSlotVisitorInlines.h"
#include "JSCInlines.h"
#include "PyTuple.h"
#include "SlotVisitorInlines.h"
#include <wtf/HashMap.h>

namespace JSC {
namespace Python {

// In what order finalizers run.
//
// With reference counts an object is finalized before what it alone refers to: the references it holds are dropped after its
// finalizer has run, and if the finalizer has found the object a new home, they are not dropped at all. Programs count on it: a
// __del__ that flushes self.file expects the file to be open.
//
// A collection finds all the unreachable objects at once. So it looks at how those with finalizers (the candidates) reach one
// another through what else is unreachable, and works out
//  - their ranks: how many others must come before. Objects that reach one another, a cycle, have the same rank;
//  - among those of the same rank, the order in which reference counts would have fallen to zero: an object lets go of what
//    it refers to in a certain order (a list from its end), and what dies of that lets go of its own before the next is let go of;
//  - what is in a cycle or hangs off one. That is what reference counts would not have freed, and CPython's collector treats it
//    differently in one respect: see Runtime::keepUnreachableObjectsToFinalize().

namespace {

// What the collection has left unmarked and the candidates reach, as a graph. Marks nothing.
class UnreachableGraph final : public AbstractSlotVisitor {
public:
    UnreachableGraph(SlotVisitor& marker, std::span<JSCell* const> candidates)
        : AbstractSlotVisitor(marker.collector(), "PythonFinalizationOrder"_s, m_opaqueRootStorage)
        , m_marker(marker)
        , m_candidateCount(candidates.size())
    {
        for (JSCell* cell : candidates)
            m_indices.add(cell, add(cell));
    }

    FinalizationOrder order(bool wantsCyclicGarbage);

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
    void visitAsConstraint(const JSCell* cell) final { cell->methodTable()->visitChildren(const_cast<JSCell*>(cell), *this); }
    void addParallelConstraintTask(RefPtr<SharedTask<void(AbstractSlotVisitor&)>>) final { }
    void addParallelConstraintTask(RefPtr<SharedTask<void(SlotVisitor&)>>) final { }

private:
    static constexpr unsigned none = std::numeric_limits<unsigned>::max();
    // The graph takes some 70 bytes for a cell, while the world is stopped. Beyond this, what is reached is not looked into: it is
    // taken to refer to nothing, and the order is only as good as what has been seen.
    static constexpr unsigned maxCells = 4'000'000;

    unsigned add(JSCell* cell)
    {
        m_cells.append(cell);
        return m_cells.size() - 1;
    }

    void reach(JSCell* cell)
    {
        if (!cell || m_marker.isMarked(cell))
            return;
        auto result = m_indices.add(cell, 0);
        if (result.isNewEntry)
            result.iterator->value = add(cell);
        m_edges.append(result.iterator->value);
    }

    std::span<const unsigned> successors(unsigned node) const { return m_edges.span().subspan(m_firstEdge[node], m_firstEdge[node + 1] - m_firstEdge[node]); }

    void walk();
    void findComponents();
    Vector<unsigned> orderOfRelease();

    SlotVisitor& m_marker;
    unsigned m_candidateCount;
    ConcurrentPtrHashSet m_opaqueRootStorage;
    // The candidates come first.
    Vector<JSCell*> m_cells;
    UncheckedKeyHashMap<JSCell*, unsigned> m_indices;
    // Those of cell i are m_edges[m_firstEdge[i]] to m_edges[m_firstEdge[i + 1]], in the order in which it would let go of them.
    Vector<unsigned> m_edges;
    Vector<unsigned> m_firstEdge;
    // Strongly connected components. One is numbered once all that it reaches is.
    Vector<unsigned> m_component;
    unsigned m_componentCount { 0 };
};

void UnreachableGraph::walk()
{
    for (unsigned i = 0; i < m_cells.size(); ++i) {
        m_firstEdge.append(m_edges.size());
        if (i >= maxCells)
            continue;
        JSCell* cell = m_cells[i];
        cell->methodTable()->visitChildren(cell, *this);
        // Cells say what they refer to from first to last. CPython's lists and tuples let go of their items from last to first.
        JSType type = cell->type();
        if (type == ArrayType || type == DerivedArrayType || type == PyTupleType)
            std::ranges::reverse(m_edges.mutableSpan().subspan(m_firstEdge[i]));
    }
    m_firstEdge.append(m_edges.size());
}

// Tarjan's algorithm, without recursion.
void UnreachableGraph::findComponents()
{
    unsigned count = m_cells.size();
    Vector<unsigned> index;
    index.fill(none, count);
    Vector<unsigned> low;
    low.fill(0, count);
    m_component.fill(none, count);
    Vector<unsigned> stack;
    struct Frame {
        unsigned node;
        unsigned next;
    };
    Vector<Frame> frames;
    unsigned counter = 0;
    for (unsigned start = 0; start < count; ++start) {
        if (index[start] != none)
            continue;
        frames.append({ start, m_firstEdge[start] });
        index[start] = low[start] = counter++;
        stack.append(start);
        while (!frames.isEmpty()) {
            auto& frame = frames.last();
            unsigned node = frame.node;
            if (frame.next < m_firstEdge[node + 1]) {
                unsigned successor = m_edges[frame.next++];
                if (index[successor] == none) {
                    index[successor] = low[successor] = counter++;
                    stack.append(successor);
                    frames.append({ successor, m_firstEdge[successor] });
                } else if (m_component[successor] == none)
                    low[node] = std::min(low[node], index[successor]);
                continue;
            }
            if (low[node] == index[node]) {
                unsigned member;
                do {
                    member = stack.takeLast();
                    m_component[member] = m_componentCount;
                } while (member != node);
                ++m_componentCount;
            }
            frames.removeLast();
            if (!frames.isEmpty())
                low[frames.last().node] = std::min(low[frames.last().node], low[node]);
        }
    }
}

// For each candidate, its place in the order in which reference counts would have got to them, had the candidates that nothing
// here refers to been dropped in the order they were made. Those that counts would never get to come last, as they were made.
Vector<unsigned> UnreachableGraph::orderOfRelease()
{
    unsigned count = m_cells.size();
    Vector<unsigned> references;
    references.fill(0, count);
    for (unsigned target : m_edges)
        ++references[target];

    Vector<unsigned> place;
    place.fill(none, m_candidateCount);
    unsigned next = 0;
    struct Frame {
        unsigned node;
        unsigned next;
    };
    Vector<Frame> frames;
    auto release = [&](unsigned node) {
        if (node < m_candidateCount)
            place[node] = next++;
        frames.append({ node, m_firstEdge[node] });
    };
    for (unsigned start = 0; start < m_candidateCount; ++start) {
        if (references[start])
            continue;
        release(start);
        while (!frames.isEmpty()) {
            auto& frame = frames.last();
            if (frame.next == m_firstEdge[frame.node + 1]) {
                frames.removeLast();
                continue;
            }
            unsigned target = m_edges[frame.next++];
            if (!--references[target])
                release(target);
        }
    }
    for (unsigned i = 0; i < m_candidateCount; ++i) {
        if (place[i] == none)
            place[i] = next++;
    }
    return place;
}

FinalizationOrder UnreachableGraph::order(bool wantsCyclicGarbage)
{
    walk();
    findComponents();
    unsigned count = m_cells.size();

    // The cells of each component.
    Vector<unsigned> firstMember;
    firstMember.fill(0, m_componentCount + 1);
    for (unsigned node = 0; node < count; ++node)
        ++firstMember[m_component[node] + 1];
    for (unsigned c = 0; c < m_componentCount; ++c)
        firstMember[c + 1] += firstMember[c];
    Vector<unsigned> members;
    members.fill(0, count);
    {
        Vector<unsigned> cursor = firstMember;
        for (unsigned node = 0; node < count; ++node)
            members[cursor[m_component[node]]++] = node;
    }

    Vector<bool> hasCandidate;
    hasCandidate.fill(false, m_componentCount);
    for (unsigned i = 0; i < m_candidateCount; ++i)
        hasCandidate[m_component[i]] = true;

    // The rank of a component: how many components with candidates in them lie on the longest way to it.
    Vector<unsigned> rank;
    rank.fill(0, m_componentCount);
    auto forEachSuccessorComponent = [&](unsigned c, const auto& function) {
        for (unsigned m = firstMember[c]; m < firstMember[c + 1]; ++m) {
            for (unsigned successor : successors(members[m]))
                function(m_component[successor]);
        }
    };
    unsigned deepest = 0;
    for (unsigned c = m_componentCount; c--;) {
        if (hasCandidate[c])
            deepest = std::max(deepest, rank[c]);
        unsigned next = rank[c] + hasCandidate[c];
        forEachSuccessorComponent(c, [&](unsigned other) {
            if (other != c)
                rank[other] = std::max(rank[other], next);
        });
    }

    // What another candidate reaches waits: it is kept alive by that one now, and comes up again in the collection that finds
    // that one gone, unless its finalizer found it a home. That takes a collection per level. A structure that is deeper than a
    // few levels (a linked list of objects with finalizers) is finalized in one round.
    constexpr unsigned deepestToTakeLevelByLevel = 3;
    bool takesAll = deepest > deepestToTakeLevelByLevel;

    FinalizationOrder result;
    Vector<unsigned> place = orderOfRelease();
    Vector<unsigned> inOrder(m_candidateCount, [](size_t i) {
        return static_cast<unsigned>(i);
    });
    std::ranges::sort(inOrder, [&](unsigned a, unsigned b) {
        return place[a] < place[b];
    });
    // The order of release has an object before what it alone refers to. Not so in and below cycles, where the ranks say more.
    if (takesAll) {
        std::ranges::stable_sort(inOrder, [&](unsigned a, unsigned b) {
            return rank[m_component[a]] < rank[m_component[b]];
        });
    }
    for (unsigned i : inOrder) {
        if (takesAll || !rank[m_component[i]])
            result.now.append(i);
        else
            result.later.append(i);
    }

    if (wantsCyclicGarbage) {
        Vector<bool> isOfCycle;
        isOfCycle.fill(false, m_componentCount);
        for (unsigned c = m_componentCount; c--;) {
            bool isCycle = firstMember[c + 1] - firstMember[c] > 1;
            forEachSuccessorComponent(c, [&](unsigned other) {
                isCycle |= other == c;
            });
            if (isCycle && hasCandidate[c] && (takesAll || !rank[c]))
                isOfCycle[c] = true;
            if (!isOfCycle[c])
                continue;
            forEachSuccessorComponent(c, [&](unsigned other) {
                isOfCycle[other] = true;
            });
            for (unsigned m = firstMember[c]; m < firstMember[c + 1]; ++m)
                result.cyclicGarbage.add(m_cells[members[m]]);
        }
    }
    return result;
}

} // namespace

FinalizationOrder finalizationOrder(SlotVisitor& marker, std::span<JSCell* const> candidates, bool wantsCyclicGarbage)
{
    if (candidates.size() < 2 && !wantsCyclicGarbage) {
        FinalizationOrder result;
        if (!candidates.empty())
            result.now.append(0);
        return result;
    }
    return UnreachableGraph(marker, candidates).order(wantsCyclicGarbage);
}

} // namespace Python
} // namespace JSC
