/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyInternals.h"

#include "AbstractSlotVisitorInlines.h"
#include "BlockDirectoryInlines.h"
#include "JSCInlines.h"
#include "PyException.h"
#include "PyFunction.h"
#include "PyGenerator.h"
#include "PySet.h"
#include "PyTuple.h"
#include "SlotVisitorInlines.h"
#include <wtf/HashMap.h>

namespace JSC {
namespace Python {

// What reference counts would have done with what a collection has found dead.
//
// With reference counts an object is finalized before what it alone refers to: the references it holds are dropped after its
// finalizer has run, and if the finalizer has found the object a new home, they are not dropped at all. Programs count on it: a
// __del__ that flushes self.file expects the file to be open. And what is in a cycle, or hangs off one, is not freed by reference
// counts at all. That is left to CPython's collector, which has rules of its own, and says how much it found.
//
// A collection of the engine's finds all the unreachable objects at once. So this looks at how the dead cells refer to one
// another, and lets go of them as reference counts would: first of those that nothing dead refers to, each of which lets go
// of what it refers to in a certain order (a list from its end), and what dies of that lets go of its own before the next is
// let go of. That gives
//  - the order in which the objects with finalizers (the candidates) would have died;
//  - their ranks: how many other candidates come before on the way to them. Those wait (see order());
//  - what is left in the end: the cycles and what hangs off them. CPython's collector finalizes that in the order in which the
//    objects were made, whatever refers to what, after it has cleared some of the weak references
//    (see Runtime::keepUnreachableObjectsToFinalize()), and that is what gc.collect() counts.
//
// A collection that the program has asked for looks at all that has died in it. That costs time for each dead cell, which is
// what a tracing collector is there to avoid, so one of the engine's own looks only at what the candidates reach. It does
// not see what refers to the candidates then: those that nothing else decides about go in the order they were made, and a
// candidate that hangs off a cycle of objects without finalizers counts as freed by reference counts.

namespace {

class DeadGraph final : public AbstractSlotVisitor {
public:
    DeadGraph(JSC::Heap& heap, SlotVisitor& marker, std::span<JSCell* const> candidates, const FinalizationQuestions& questions)
        : AbstractSlotVisitor(marker.collector(), "PythonFinalizationOrder"_s, m_opaqueRootStorage)
        , m_heap(heap)
        , m_marker(marker)
        , m_questions(questions)
        , m_candidateCount(candidates.size())
        , m_hasAllThatDied(questions.looksAtAllThatDied)
    {
        for (JSCell* cell : candidates)
            add(cell);
        if (m_hasAllThatDied)
            addAllThatDied(candidates);
    }

    FinalizationOrder order();

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
    // The graph takes some 60 bytes for a cell, while the world is stopped. If more than this has died, only what the candidates
    // reach is looked at, and beyond as many cells of that, what is reached is not looked into: it is taken to refer to nothing.
    static constexpr unsigned maxCells = 4'000'000;

    enum Flag : uint8_t {
        Released = 1 << 0, // Reference counts would have freed it.
        Waits = 1 << 1, // It is left, and a candidate that reference counts would have freed reaches it.
        IsKept = 1 << 2, // A candidate reaches it.
        HasLetGo = 1 << 3,
    };

    // These refer to nothing but their like.
    static bool isLeaf(JSCell* cell)
    {
        JSType type = cell->type();
        return type == StringType || type == HeapBigIntType || type == SymbolType || type == PyFloatType;
    }

    // Cells say what they refer to from first to last. CPython's lists and tuples let go of their items from last to first, and so
    // do frames of their variables.
    static bool letsGoFromTheEnd(JSCell* cell)
    {
        switch (cell->type()) {
        case ArrayType:
        case DerivedArrayType:
        case PyGeneratorType:
            return true;
        case PyTupleType:
            return true;
        default:
            return false;
        }
    }

    // The memory of a dict or a set.
    static bool isTableOfContainer(JSCell* cell)
    {
        return cell->type() == CellType && (cell->classInfo() == PyDictStorage::info() || cell->classInfo() == PySetStorage::info());
    }

    // Where the number of a cell is kept: there is a table for each block that has any, with room for as many cells as the block has.
    struct Table {
        unsigned first { 0 }; // In m_numbers.
        uint16_t startAtom { 0 };
        uint16_t atomsPerCell { 1 };
    };

    unsigned* slotFor(JSCell* cell, bool makesRoom)
    {
        if (cell->isPreciseAllocation()) {
            if (makesRoom)
                return &m_numbersOfPreciseAllocations.add(cell, none).iterator->value;
            auto it = m_numbersOfPreciseAllocations.find(cell);
            return it == m_numbersOfPreciseAllocations.end() ? nullptr : &it->value;
        }
        MarkedBlock& block = cell->markedBlock();
        if (&block != m_lastBlock) {
            auto it = m_tables.find(&block);
            if (it != m_tables.end())
                m_tableOfLastBlock = it->value;
            else {
                if (!makesRoom)
                    return nullptr;
                MarkedBlock::Handle& handle = block.handle();
                size_t startAtom = block.atomNumber(handle.start());
                size_t atomsPerCell = handle.cellSize() / MarkedBlock::atomSize;
                m_tableOfLastBlock = { static_cast<unsigned>(m_numbers.size()), static_cast<uint16_t>(startAtom), static_cast<uint16_t>(atomsPerCell) };
                m_numbers.grow(m_numbers.size() + (MarkedBlock::endAtom - startAtom) / atomsPerCell);
                std::ranges::fill(m_numbers.mutableSpan().subspan(m_tableOfLastBlock.first), none);
                m_tables.add(&block, m_tableOfLastBlock);
            }
            m_lastBlock = &block;
        }
        return &m_numbers[m_tableOfLastBlock.first + (block.atomNumber(cell) - m_tableOfLastBlock.startAtom) / m_tableOfLastBlock.atomsPerCell];
    }

    void add(JSCell* cell)
    {
        unsigned* slot = slotFor(cell, true);
        if (*slot != none)
            return;
        *slot = m_cells.size();
        m_cells.append(cell);
        m_references.append(0);
    }

    void reach(JSCell* cell)
    {
        if (!cell || cell == m_wholeOfCurrentCell || m_marker.isMarked(cell))
            return;
#if ASSERT_ENABLED
        // What a cell that was alive until now refers to was alive until now.
        if (!cell->structureID().decontaminate()) [[unlikely]] {
            dataLogLn("A cell of class ", m_currentCell->classInfo()->className, m_currentCell->type() == PyObjectType ? makeString(" and type "_s, uncheckedDowncast<PyObject>(m_currentCell)->type()->name()) : String(), " refers to a cell that has been freed");
            RELEASE_ASSERT_NOT_REACHED();
        }
#endif
        unsigned* slot = slotFor(cell, !m_hasAllThatDied);
        if (!slot)
            return;
        if (*slot == none) {
            if (m_hasAllThatDied || isLeaf(cell))
                return;
            *slot = m_cells.size();
            m_cells.append(cell);
            m_references.append(0);
        }
        m_edges.append(*slot);
        ++m_references[*slot];
    }

    // Whether reach() has made an edge of it, for a value that the cell that has just been visited holds.
    bool hasEdgeTo(JSValue value)
    {
        if (!value || !value.isCell() || value.asCell() == m_wholeOfCurrentCell || m_marker.isMarked(value.asCell()))
            return false;
        unsigned* slot = slotFor(value.asCell(), false);
        return slot && *slot != none;
    }

    std::span<const unsigned> successors(unsigned node) const { return m_edges.span().subspan(m_firstEdge[node], m_firstEdge[node + 1] - m_firstEdge[node]); }
    bool isCandidate(unsigned node) const { return node < m_candidateCount; }

    template<typename Function> void forEachCellThatDied(MarkedSpace&, MarkedBlock::Handle*, const Function&);
    void addAllThatDied(std::span<JSCell* const> candidates);
    void putAttributesInOrder(PyInstance*, std::span<unsigned> edges);
    void walk();
    Vector<unsigned> rootsInOrder();
    void flagAllReachedFrom(Vector<unsigned>&& worklist, Flag);

    JSC::Heap& m_heap;
    SlotVisitor& m_marker;
    const FinalizationQuestions& m_questions;
    unsigned m_candidateCount;
    bool m_hasAllThatDied;
    size_t m_cellsLookedAt { 0 };
    JSCell* m_wholeOfCurrentCell { nullptr };
#if ASSERT_ENABLED
    JSCell* m_currentCell { nullptr };
#endif
    ConcurrentPtrHashSet m_opaqueRootStorage;

    UncheckedKeyHashMap<MarkedBlock*, Table> m_tables;
    Vector<unsigned> m_numbers;
    UncheckedKeyHashMap<JSCell*, unsigned> m_numbersOfPreciseAllocations;
    MarkedBlock* m_lastBlock { nullptr };
    Table m_tableOfLastBlock;

    // The candidates come first.
    Vector<JSCell*> m_cells;
    // Those of cell i are m_edges[m_firstEdge[i]] to m_edges[m_firstEdge[i + 1]], in the order in which it would let go of them.
    Vector<unsigned> m_edges;
    Vector<unsigned> m_firstEdge;
    Vector<unsigned> m_references; // How many edges lead to it from cells that have not let go yet.
    Vector<uint8_t> m_flags;
};

// function(cell) for the cells of the block that were alive when the collection began and are not marked. This is
// MarkedBlock::Handle::isLive() for a whole block, with the world stopped. (In a collection of the young generation the marks of
// the last collection stand, so that what is old counts as alive.)
template<typename Function>
void DeadGraph::forEachCellThatDied(MarkedSpace& space, MarkedBlock::Handle* handle, const Function& function)
{
    MarkedBlock& block = handle->block();
    bool marksAreCurrent = block.markingVersion() == space.markingVersion();
    auto cellAt = [&](size_t atom) {
        return reinterpret_cast<JSCell*>(reinterpret_cast<char*>(&block) + atom * MarkedBlock::atomSize);
    };
    if (handle->isAllocated()) {
        size_t atomsPerCell = handle->cellSize() / MarkedBlock::atomSize;
        for (size_t atom = block.atomNumber(handle->start()); atom < MarkedBlock::endAtom; atom += atomsPerCell) {
            if (!marksAreCurrent || !block.marks().get(atom))
                function(cellAt(atom));
        }
        return;
    }
    WTF::BitSet<MarkedBlock::atomsPerBlock> dead;
    if (block.newlyAllocatedVersion() == space.newlyAllocatedVersion()) {
        dead = block.newlyAllocated();
        if (marksAreCurrent)
            dead.exclude(block.marks());
    } else if (!marksAreCurrent && MarkedSpace::nextVersion(block.markingVersion()) == space.markingVersion() && m_heap.collectionScope() == CollectionScope::Full)
        dead = block.marks(); // Nothing in it has been marked. These are the survivors of the last collection.
    else
        return;
    dead.forEachSetBit([&](size_t atom) {
        function(cellAt(atom));
    });
}

void DeadGraph::addAllThatDied(std::span<JSCell* const> candidates)
{
    auto look = [&](JSCell* cell) {
        ++m_cellsLookedAt;
        if (cell->structureID() && !isLeaf(cell) && m_cells.size() <= maxCells)
            add(cell);
    };
    MarkedSpace& space = m_heap.objectSpace();
    bool isOfYoungGeneration = m_heap.collectionScope() == CollectionScope::Eden;
    space.forEachBlock([&](MarkedBlock::Handle* handle) {
        if (!isJSCellKind(handle->cellKind()))
            return;
        if (isOfYoungGeneration) {
            // Most blocks have nothing young in them, and are not even touched.
            BlockDirectory* directory = handle->directory();
            directory->assertIsMutatorOrMutatorIsStopped();
            if (!directory->isEden(handle))
                return;
        }
        m_cellsLookedAt += handle->markCount();
        forEachCellThatDied(space, handle, [&](JSCell* cell) {
#if ASAN_ENABLED
            if (handle->isRedZoneCell(cell))
                return;
#endif
            look(cell);
        });
    });
    // What an earlier collection found dead may still be in the list: not swept yet (Heap::mayRunEmbedderFinalizers()), or swept, so
    // that there is no cell any more, and kept for the weak handles on it. See PreciseAllocation::flip() for what the bits say
    // while a full collection marks.
    for (PreciseAllocation* allocation : space.preciseAllocations()) {
        if (!isJSCellKind(allocation->attributes().cellKind))
            continue;
        if (allocation->isMarked())
            ++m_cellsLookedAt;
        else if (allocation->isNewlyAllocated() && allocation->hasValidCell())
            look(static_cast<JSCell*>(allocation->cell()));
    }
    if (m_cells.size() <= maxCells)
        return;

    m_hasAllThatDied = false;
    m_tables.clear();
    m_numbers.clear();
    m_numbersOfPreciseAllocations.clear();
    m_lastBlock = nullptr;
    m_cells.clear();
    m_references.clear();
    for (JSCell* cell : candidates)
        add(cell);
}

// An instance lets go of its attributes in the order in which they were set. It says what it refers to as every object of the engine does: first what has no room in the cell itself, from last to
// first, then what is in the cell. This puts them in the order of their places, which is the order in which they were set unless one has been deleted: the engine gives its place to the next that
// is set. (The order of setting is in the property table, which a collection drops, and to make it anew allocates.)
void DeadGraph::putAttributesInOrder(PyInstance* instance, std::span<unsigned> edges)
{
    Structure* structure = instance->structure();
    size_t outside = 0;
    for (unsigned i = 0; i < structure->outOfLineSize(); ++i)
        outside += hasEdgeTo(instance->getDirect(firstOutOfLineOffset + i));
    if (!outside)
        return;
    size_t before = hasEdgeTo(structure);
    if (before + outside > edges.size())
        return;
    edges = edges.subspan(before);
    std::ranges::reverse(edges.first(outside));
    std::ranges::rotate(edges, edges.begin() + outside);
}

void DeadGraph::walk()
{
    UncheckedKeyHashSet<unsigned, IntHash<unsigned>, WTF::UnsignedWithZeroKeyHashTraits<unsigned>> memoryOfOthers; // A deque lets go from the left.
    struct Ring {
        unsigned node;
        unsigned head;
    };
    Vector<Ring> rings;
    for (unsigned i = 0; i < m_cells.size(); ++i) {
        m_firstEdge.append(m_edges.size());
        JSCell* cell = m_cells[i];
        if (i >= maxCells || isInternalObject(cell))
            continue;
        m_wholeOfCurrentCell = wholeThatPartRefersTo(cell);
#if ASSERT_ENABLED
        m_currentCell = cell;
        if (!cell->structureID().decontaminate()) [[unlikely]] {
            auto* words = reinterpret_cast<uint64_t*>(cell);
            dataLogLn("Cell ", i, " of the dead (", m_candidateCount, " candidates, ", m_hasAllThatDied ? "all that died" : "what they reach", ") has been freed: JSType ", static_cast<unsigned>(cell->type()), ", ", RawHex(words[0]), " ", RawHex(words[1]), " ", RawHex(words[2]), " ", RawHex(words[3]), " ", RawHex(words[4]), " ", RawHex(words[5]));
            RELEASE_ASSERT_NOT_REACHED();
        }
#endif
        cell->methodTable()->visitChildren(cell, *this);
        if (cell->type() == PyInstanceType)
            putAttributesInOrder(uncheckedDowncast<PyInstance>(cell), m_edges.mutableSpan().subspan(m_firstEdge[i]));
        else if (cell->type() == PyObjectType && uncheckedDowncast<PyObject>(cell)->type()->has(TypeFlag::OwnsContainers)) {
            for (unsigned part : m_edges.span().subspan(m_firstEdge[i]))
                memoryOfOthers.add(part);
            unsigned head = 0;
            if (PyTuple* ring = ringOfDeque(uncheckedDowncast<PyRecord>(cell), head); ring && head && hasEdgeTo(ring))
                rings.append({ *slotFor(ring, false), head });
        }
    }
    m_firstEdge.append(m_edges.size());
    for (unsigned i = 0; i < m_cells.size(); ++i) {
        if (letsGoFromTheEnd(m_cells[i]) && !memoryOfOthers.contains(i))
            std::ranges::reverse(m_edges.mutableSpan().subspan(m_firstEdge[i], m_firstEdge[i + 1] - m_firstEdge[i]));
    }
    // The leftmost item of a deque is somewhere in the middle of its memory.
    m_wholeOfCurrentCell = nullptr;
    for (auto [node, head] : rings) {
        auto countEdges = [&](std::span<const JSValue> items) {
            size_t count = 0;
            for (JSValue item : items)
                count += hasEdgeTo(item);
            return count;
        };
        auto items = uncheckedDowncast<PyTuple>(m_cells[node])->span();
        size_t wrapped = countEdges(items.first(head));
        size_t all = wrapped + countEdges(items.subspan(head));
        auto edges = m_edges.mutableSpan().subspan(m_firstEdge[node], m_firstEdge[node + 1] - m_firstEdge[node]);
        // The items come last. (In front of them: its Structure and its type, if these have died with it.)
        if (all <= edges.size()) {
            edges = edges.last(all);
            std::ranges::rotate(edges, edges.begin() + wrapped);
        }
    }
}

// The cells that nothing dead refers to: the program dropped them, in an order that nobody knows. They are put in the order in which
// the first candidate that each reaches was made. Those that reach none come last.
Vector<unsigned> DeadGraph::rootsInOrder()
{
    unsigned count = m_cells.size();
    Vector<unsigned> roots;
    if (!m_hasAllThatDied) {
        // All there is has been reached from a candidate.
        for (unsigned i = 0; i < m_candidateCount; ++i) {
            if (!m_references[i])
                roots.append(i);
        }
        return roots;
    }

    Vector<unsigned> firstCandidateReached;
    if (m_candidateCount) {
        Vector<unsigned> firstPredecessor;
        firstPredecessor.reserveInitialCapacity(count + 1);
        unsigned sum = 0;
        for (unsigned references : m_references) {
            firstPredecessor.append(sum);
            sum += references;
        }
        firstPredecessor.append(sum);
        Vector<unsigned> predecessors;
        predecessors.fill(0, sum);
        {
            Vector<unsigned> cursor = firstPredecessor;
            for (unsigned node = 0; node < count; ++node) {
                for (unsigned successor : successors(node))
                    predecessors[cursor[successor]++] = node;
            }
        }
        firstCandidateReached.fill(none, count);
        Vector<unsigned> worklist;
        for (unsigned i = 0; i < m_candidateCount; ++i) {
            if (firstCandidateReached[i] != none)
                continue;
            firstCandidateReached[i] = i;
            worklist.append(i);
            while (!worklist.isEmpty()) {
                unsigned node = worklist.takeLast();
                for (unsigned p = firstPredecessor[node]; p < firstPredecessor[node + 1]; ++p) {
                    unsigned predecessor = predecessors[p];
                    if (firstCandidateReached[predecessor] != none)
                        continue;
                    firstCandidateReached[predecessor] = i;
                    worklist.append(predecessor);
                }
            }
        }
        for (unsigned node = 0; node < count; ++node) {
            if (!m_references[node] && firstCandidateReached[node] != none)
                roots.append(node);
        }
        std::ranges::stable_sort(roots, [&](unsigned a, unsigned b) {
            return firstCandidateReached[a] < firstCandidateReached[b];
        });
        // A table that nothing refers to: its dict or set has left it behind for a larger one, and it still holds what it held then. With reference counts it was freed there and then and let go of
        // nothing. So these go first: what they let go of is still held by the table that came after, and goes when that does. (Or the container has been emptied: then this is how it let go.)
        std::ranges::stable_partition(roots, [&](unsigned node) {
            return isTableOfContainer(m_cells[node]);
        });
    }
    for (unsigned node = 0; node < count; ++node) {
        if (!m_references[node] && (!m_candidateCount || firstCandidateReached[node] == none))
            roots.append(node);
    }
    return roots;
}

// What is in the worklist has the flag already.
void DeadGraph::flagAllReachedFrom(Vector<unsigned>&& worklist, Flag flag)
{
    while (!worklist.isEmpty()) {
        unsigned node = worklist.takeLast();
        for (unsigned successor : successors(node)) {
            if (m_flags[successor] & flag)
                continue;
            m_flags[successor] |= flag;
            worklist.append(successor);
        }
    }
}

FinalizationOrder DeadGraph::order()
{
    walk();
    unsigned count = m_cells.size();
    m_flags.fill(0, count);
    FinalizationOrder result;
    result.hasLookedAtAllThatDied = m_hasAllThatDied;
    result.cellsLookedAt = m_cellsLookedAt;

    // What reference counts would have freed, in their order.
    // The rank of a cell: how many candidates lie on the longest way to it.
    Vector<unsigned> rank;
    rank.fill(0, count);
    Vector<unsigned> released; // The candidates among it.
    unsigned deepest = 0;
    // The candidate whose going frees a cell: the nearest on the way by which the last reference to it is dropped.
    Vector<unsigned> letGoBy;
    letGoBy.fill(none, count);
    {
        struct Frame {
            unsigned node;
            unsigned next;
        };
        Vector<Frame> frames;
        auto release = [&](unsigned node) {
            m_flags[node] |= Released;
            if (isCandidate(node)) {
                released.append(node);
                deepest = std::max(deepest, rank[node]);
            }
            frames.append({ node, m_firstEdge[node] });
        };
        for (unsigned root : rootsInOrder()) {
            release(root);
            while (!frames.isEmpty()) {
                auto& frame = frames.last();
                if (frame.next == m_firstEdge[frame.node + 1]) {
                    frames.removeLast();
                    continue;
                }
                unsigned target = m_edges[frame.next++];
                rank[target] = std::max(rank[target], rank[frame.node] + isCandidate(frame.node));
                if (!--m_references[target]) {
                    letGoBy[target] = isCandidate(frame.node) ? frame.node : letGoBy[frame.node];
                    release(target);
                }
            }
        }
    }
    static_assert(none == FinalizationOrder::none);
    result.letGoBy = Vector<unsigned>(letGoBy.span().first(m_candidateCount));

    // What another candidate reaches waits: it is kept alive by that one now, and comes up again in the collection that finds
    // that one gone, unless its finalizer found it a home. That takes a collection per level. A structure that is deeper than a
    // few levels (a linked list of objects with finalizers) is finalized in one round.
    constexpr unsigned deepestToTakeLevelByLevel = 3;
    bool takesAll = deepest > deepestToTakeLevelByLevel;
    for (unsigned node : released) {
        if (takesAll || !rank[node])
            result.now.append(node);
        else
            result.later.append(node);
    }
    result.freedByReferenceCounts = result.now.size();
    result.takesAllLevels = takesAll;

    // The rest is for CPython's collector, which comes later, and calls the finalizers of all that it finds in one go, in the
    // order in which the objects were made.
    if (!takesAll) {
        Vector<unsigned> worklist;
        for (unsigned node = 0; node < count; ++node) {
            if (!(m_flags[node] & Released) && rank[node]) {
                m_flags[node] |= Waits;
                worklist.append(node);
            }
        }
        flagAllReachedFrom(WTF::move(worklist), Waits);
    }
    for (unsigned i = 0; i < m_candidateCount; ++i) {
        if (m_flags[i] & Released)
            continue;
        if (m_flags[i] & Waits)
            result.later.append(i);
        else
            result.now.append(i);
    }

    // What is left falls into pieces that have nothing to do with one another.
    Vector<unsigned> piece;
    auto pieceOf = [&](unsigned node) {
        while (piece[node] != node)
            node = piece[node] = piece[piece[node]];
        return node;
    };
    if (m_questions.wantsPieces) {
        piece = Vector<unsigned>(count, [](size_t i) {
            return static_cast<unsigned>(i);
        });
        for (unsigned node = 0; node < count; ++node) {
            if (m_flags[node] & Released)
                continue;
            for (unsigned successor : successors(node)) {
                // The lower number stands for the piece: that of its oldest candidate.
                if (unsigned a = pieceOf(node), b = pieceOf(successor); !(m_flags[successor] & Released) && a != b)
                    piece[std::max(a, b)] = std::min(a, b);
            }
        }
    }

    if (m_hasAllThatDied) {
        Vector<unsigned> worklist;
        for (unsigned i = 0; i < m_candidateCount; ++i) {
            m_flags[i] |= IsKept;
            worklist.append(i);
        }
        flagAllReachedFrom(WTF::move(worklist), IsKept);
    } else {
        for (auto& flags : m_flags)
            flags |= IsKept;
    }

    for (JSCell* cell : m_questions.askedAbout) {
        unsigned* slot = slotFor(cell, false);
        bool isGarbage = slot && *slot != none && (m_flags[*slot] & (Released | Waits | IsKept)) == IsKept;
        result.isCyclicGarbageToFinalize.append(isGarbage);
        if (m_questions.wantsPieces)
            result.oldestOfPieceOfCellAskedAbout.append(isGarbage ? pieceOf(*slot) : none);
    }
    if (m_questions.wantsPieces) {
        for (unsigned i = 0; i < m_candidateCount; ++i)
            result.oldestOfPiece.append(m_flags[i] & Released ? none : pieceOf(i));
    }

    if (!m_questions.wantsGarbage || !m_hasAllThatDied)
        return result;

    // CPython's collector closes the generators it finds, which then refer to nothing: if that was all that held a cycle
    // together, reference counts do the rest, and it is not counted. (One with something to do when it is closed is a candidate.)
    Vector<unsigned> worklist;
    auto letGo = [&](unsigned node) {
        if (m_flags[node] & HasLetGo)
            return;
        m_flags[node] |= HasLetGo;
        for (unsigned successor : successors(node)) {
            if (!--m_references[successor])
                worklist.append(successor);
        }
    };
    for (unsigned node = 0; node < count; ++node) {
        if (!(m_flags[node] & (Released | IsKept)) && m_cells[node]->type() == PyGeneratorType)
            letGo(node);
    }
    while (!worklist.isEmpty()) {
        unsigned node = worklist.takeLast();
        m_flags[node] |= Released;
        letGo(node);
    }
    for (unsigned node = 0; node < count; ++node) {
        if (!(m_flags[node] & (Released | IsKept)))
            result.garbage.append(m_cells[node]);
    }
    return result;
}

} // namespace

JSCell* wholeThatPartRefersTo(JSCell* cell)
{
    // The function that runs the body of the generators of a generator function.
    if (cell->type() == JSFunctionType) {
        auto* function = PyFunction::tryFrom(cell);
        PyFunction* owner = function ? function->codeOwner() : nullptr;
        return owner != function ? owner : nullptr;
    }
    // gi_frame. In CPython it outlives the generator.
    if (cell->type() == PyObjectType) {
        if (auto* frame = PyFrame::tryFrom(cell))
            return frame->generator();
        // The code keeps its code object, once there is one.
        if (auto* code = PyCode::tryFrom(cell))
            return code->executable()->pythonCodeObject() == code ? code->executable() : nullptr;
    }
    return nullptr;
}

FinalizationOrder finalizationOrder(JSC::Heap& heap, SlotVisitor& marker, std::span<JSCell* const> candidates, const FinalizationQuestions& questions)
{
    if (candidates.size() < 2 && !questions.wantsGarbage && !questions.wantsToKnowWhatReferenceCountsWouldFree && questions.askedAbout.empty()) {
        FinalizationOrder result;
        if (!candidates.empty())
            result.now.append(0);
        result.freedByReferenceCounts = result.now.size();
        return result;
    }
    return DeadGraph(heap, marker, candidates, questions).order();
}

} // namespace Python
} // namespace JSC
