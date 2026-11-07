/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyInternals.h"
#include "PyModule.h"

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
//    objects were made, whatever refers to what, unless an earlier collection has seen them alive (candidatesWithHoldersFirst()),
//    after it has cleared some of the weak references (see Runtime::keepUnreachableObjectsToFinalize()), and that is what
//    gc.collect() counts.
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
        , m_marker(&marker)
        , m_questions(questions)
        , m_candidateCount(candidates.size())
        , m_hasAllThatDied(questions.looksAtAllThatDied)
    {
        for (JSCell* cell : candidates)
            add(cell);
        if (m_hasAllThatDied)
            addAllThatDied(candidates);
    }

    // Outside a collection, of cells that are alive: the graph is what the candidates lead to by way of cells of the program, but not by way of a namespace: by its class, the methods of that and their
    // globals every instance leads to all that is alive. The namespaces that are passed by are noted, and of what is in them only that is looked at which is in the graph anyway: it has a name.
    // No cell is allocated while there is one.
    DeadGraph(JSC::Heap& heap, std::span<JSCell* const> candidates, const FinalizationQuestions& questions)
        : AbstractSlotVisitor(heap.collector(), "PythonFinalizationOrder"_s, m_opaqueRootStorage)
        , m_heap(heap)
        , m_questions(questions)
        , m_candidateCount(candidates.size())
        , m_hasAllThatDied(false)
    {
        for (JSCell* cell : candidates)
            add(cell);
    }

    FinalizationOrder order();
    Vector<unsigned> orderOfTheLiving();

    void append(const ConservativeRoots&) final { }
    void appendUnbarriered(JSCell* cell) final { reach(cell); }
    void appendHiddenUnbarriered(JSCell* cell) final { reach(cell); }
    bool isFirstVisit() const final { return false; }
    bool isMarked(const void* cell) const final { return !m_marker || m_marker->isMarked(cell); }
    bool isMarked(MarkedBlock& block, HeapCell* cell) const final { return !m_marker || m_marker->isMarked(block, cell); }
    bool isMarked(PreciseAllocation& allocation, HeapCell* cell) const final { return !m_marker || m_marker->isMarked(allocation, cell); }
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
    // The graph takes some 60 bytes for a cell, while the world is stopped, and when the modules have gone some 36 more (48 for a cell in a circle, if cells of a circle that is opened count as one), and up to 300 MB for the largest circle in which a candidate is
    // (candidatesWithHoldersFirst()). If more than this has died, only what the candidates reach is looked at, and beyond as many cells of that, what is reached is not looked into: it is taken to refer to nothing.
    static constexpr unsigned maxCells = 4'000'000;
    // See candidatesWithHoldersFirst().
    static constexpr unsigned mostCellsOfSmallCircle = 64;

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

    // The memory of a dict or a set that has gone on to other memory since: see PyDictStorage::wasLeftForAnother().
    static bool isTableLeftForAnother(JSCell* cell)
    {
        if (cell->type() != CellType)
            return false;
        if (cell->classInfo() == PyDictStorage::info())
            return uncheckedDowncast<PyDictStorage>(cell)->wasLeftForAnother();
        return cell->classInfo() == PySetStorage::info() && uncheckedDowncast<PySetStorage>(cell)->wasLeftForAnother();
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

    // Not of the graph.
    bool isOutside(JSCell* cell) const { return m_marker ? m_marker->isMarked(cell) : !isCellOfTheProgram(cell); }

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
        if (!cell || cell == m_wholeOfCurrentCell || cell == m_namespacesOfCurrentCell[0] || cell == m_namespacesOfCurrentCell[1] || isOutside(cell))
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
        if (!value || !value.isCell() || value.asCell() == m_wholeOfCurrentCell || isOutside(value.asCell()))
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
    Vector<unsigned> candidatesWithHoldersFirst();
    void flagAllReachedFrom(Vector<unsigned>&& worklist, Flag);

    JSC::Heap& m_heap;
    SlotVisitor* m_marker { nullptr };
    const FinalizationQuestions& m_questions;
    unsigned m_candidateCount;
    bool m_hasAllThatDied;
    size_t m_cellsLookedAt { 0 };
    JSCell* m_wholeOfCurrentCell { nullptr };
    std::array<JSCell*, 2> m_namespacesOfCurrentCell { }; // Among the living only, like the next.
    UncheckedKeyHashSet<PyDict*> m_namespacesPassedBy;
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
        if (i >= maxCells || isInternalObject(cell) || isTableLeftForAnother(cell))
            continue;
        m_wholeOfCurrentCell = wholeThatPartRefersTo(cell);
        if (!m_marker) {
            m_namespacesOfCurrentCell = { };
            if (auto* function = cell->type() == JSFunctionType ? PyFunction::tryFrom(cell) : nullptr)
                m_namespacesOfCurrentCell = { function->globals(), function->builtins() };
            else if (auto* module = PyModule::tryFrom(cell))
                m_namespacesOfCurrentCell[0] = module->dict();
            for (JSCell* dict : m_namespacesOfCurrentCell) {
                if (dict)
                    m_namespacesPassedBy.add(uncheckedDowncast<PyDict>(dict));
            }
        }
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
    }
    for (unsigned node = 0; node < count; ++node) {
        if (!m_references[node] && (!m_candidateCount || firstCandidateReached[node] == none))
            roots.append(node);
    }
    return roots;
}

// A set of numbers below `limit`, of which the lowest is wanted again and again: a bit for each number, above that a bit for each word that is not empty, and so on. (A heap of a million that are all
// there at once goes all over its memory for each one that is taken.)
class LowestFirst {
public:
    explicit LowestFirst(unsigned limit)
    {
        do {
            limit = std::max(1u, (limit + 63) / 64);
            m_levels.append(Vector<uint64_t>());
            m_levels.last().fill(0, limit);
        } while (limit > 1);
    }

    bool isEmpty() const { return !m_levels[m_levels.size() - 1][0]; }

    void add(unsigned number)
    {
        for (auto& level : m_levels) {
            uint64_t& word = level[number / 64];
            bool wasEmpty = !word;
            word |= 1ull << number % 64;
            if (!wasEmpty)
                return;
            number /= 64;
        }
    }

    unsigned takeLowest()
    {
        unsigned lowest = 0;
        for (size_t i = m_levels.size(); i--;)
            lowest = lowest * 64 + std::countr_zero(m_levels[i][lowest]);
        unsigned number = lowest;
        for (auto& level : m_levels) {
            uint64_t& word = level[number / 64];
            word &= ~(1ull << number % 64);
            if (word)
                break;
            number /= 64;
        }
        return lowest;
    }

private:
    Vector<Vector<uint64_t>, 4> m_levels;
};

// The strongly connected components of a graph (Tarjan's algorithm, without recursion). `vertices`: where to begin. In `states`, which has room for all, `number` says none for every vertex that has not
// been come to; one that is to count as found already has any other number, and none for `low`. The successors of a vertex are successor(vertex, position) for the positions from begin(vertex) to
// end(vertex). didFind(members) for each component, one that others lead to before those. `members` is good for that call only, which may write what it likes into `number` of the members, but none.
// It may be called again with the same `states`, as long as nothing has been written into `low`: what has been found is found already.
struct ComponentSearch {
    struct State {
        unsigned number;
        unsigned low;
    };
    struct Frame {
        unsigned vertex;
        unsigned next;
    };
    // Between two searches they are empty, and keep their memory.
    Vector<Frame> frames;
    Vector<unsigned> stack;
};

template<typename Vertices, typename Begin, typename End, typename Successor, typename DidFind>
void forEachComponent(ComponentSearch& search, Vertices vertices, Vector<ComponentSearch::State>& states, const Begin& begin, const End& end, const Successor& successor, const DidFind& didFind)
{
    constexpr unsigned none = std::numeric_limits<unsigned>::max();
    constexpr unsigned found = none; // In `low`: its component has been found. It counts no more.
    auto& frames = search.frames;
    auto& stack = search.stack;
    ASSERT(frames.isEmpty() && stack.isEmpty());
    unsigned clock = 0;
    auto enter = [&](unsigned vertex) {
        states[vertex] = { clock, clock };
        ++clock;
        stack.append(vertex);
        frames.append({ vertex, begin(vertex) });
    };
    for (unsigned first : vertices) {
        if (states[first].number != none) {
            ASSERT(states[first].low == found);
            continue;
        }
        enter(first);
        while (!frames.isEmpty()) {
            unsigned vertex = frames.last().vertex;
            if (frames.last().next != end(vertex)) {
                unsigned target = successor(vertex, frames.last().next++);
                auto state = states[target];
                if (state.number == none)
                    enter(target);
                else if (state.low != found)
                    states[vertex].low = std::min(states[vertex].low, state.number);
                continue;
            }
            frames.removeLast();
            if (!frames.isEmpty())
                states[frames.last().vertex].low = std::min(states[frames.last().vertex].low, states[vertex].low);
            if (states[vertex].low != states[vertex].number)
                continue;
            size_t size = 1;
            while (stack[stack.size() - size] != vertex)
                ++size;
            auto members = stack.span().last(size);
            for (unsigned member : members)
                states[member].low = found;
            didFind(members);
            stack.shrink(stack.size() - size);
        }
    }
}

// The candidates that reference counts would not have freed, in the order in which they were made, but one that something holds not before that has gone: the oldest of those that are free to go comes
// next. So what was made for its holder, as the raw file in Writer(Raw()), comes right behind it.
//
// This is for what dies when the modules go at exit. CPython's collector calls the finalizers in the order of its lists, where an object is put at the end when it is made. But a collection that
// sees an object alive, and comes to it before it has come to anything that refers to it, puts it at the end again, behind what refers to it; and CPython collects before it lets go of the modules. So
// an object that is reached only through others goes after those, in every version, however the program is started and whatever its size. The rest of its order depends on where the generations of
// its collector happen to be cut. What is at stake is data: a writer has to go before the raw file that it writes to.
//
// A reference holds what it refers to, but:
// - Nothing holds a namespace. A function refers to the namespace of its module, which refers to the function: so nearly all that a program holds at its end hangs off a cycle and, by way of its class and
//   the methods of that, leads to everything else. Namespaces: the globals and the builtins of a function (so a dict that exec() was given, if a function was defined in it), and the dicts of the modules
//   that the interpreter has let go of (FinalizationQuestions::namespaces), which need not be in the graph: the program may have no function.
// - Nothing holds a candidate that has a name in a namespace: of `child = T(); parent = T(child)` the child goes first, in CPython too, whose collector comes from the namespace to both. The name has to be
//   its own: of `child = T(); l = [child]; parent = T(child); del child` the parent goes first, there as here.
// - No cell holds itself.
// - Where cells hold one another, the circle is opened where it is entered from the names. The level of a cell: how few candidates one passes on a way to it from a namespace, from what has a name, or from what
//   nothing refers to (through what reference counts would have freed, too). Nearer to the names: the lower level; at one level what is no candidate; of two candidates the younger, which is the wrapper
//   if one of them is. A REFERENCE TO A CELL DOES NOT HOLD IT IF THE CELL LEADS BACK TO WHERE THE REFERENCE COMES FROM BY A WAY ON WHICH NOTHING IS NEARER THAN THE CELL ITSELF. So of `raw.owner = f`, where
//   the program has `f`, the writer goes first, whatever else holds the raw file, and a tree with links to the parents goes from its root down. Any other reference that is on no circle holds; what
//   holds has no circle; and nothing depends on the order in which the cells or their references are come to.
// - THE TWO ENDS OF A REFERENCE THAT DOES NOT HOLD COUNT AS ONE CELL IF THEY ARE EQUALLY NEAR, and so do all the cells of a circle that holds nothing together (see below): they are free to go, together, when no
//   reference that holds is left to any of them. Else what holds one list of a cycle of lists, or the root of a tree of plain nodes with links to the parents, would not hold what hangs off another list or a leaf,
//   and an older object there would go before it. With that, an object goes after every candidate through which all the ways to it lead, from where the levels begin; and what holds still has no circle. (Two cells
//   that are equally near and lead to one another only by way of farther ones stay two: what holds the one does not hold what hangs off the other.)
// - What no way leads to (the graph may be no more than what the candidates lead to, and among the living it never is: a list with a name that holds them is not in it) is entered at its youngest candidate: from the holders
//   down, a component that nothing has led to yet gets level 0 there (everywhere, if it has no candidate), and the levels spread from all those places. What had a level keeps it.
//
// How that is worked out. The strongly connected components. A component all of whose cells are equally near holds nothing together: none of its inner references holds. (Every component without a
// candidate is one: through what is no candidate the level does not rise.) In any other, the question for a reference from u to v is whether u and v are strongly connected among the cells that are no
// nearer than v. In a circle of up to 64 cells that is worked out as it stands: from each cell what it reaches among what is no nearer, as a set of bits. That takes two looks at each reference of its cells
// and no memory, so it is done always (a third look, if two of them are no candidates, says which are one, and that does take memory: see the end): a writer and its raw file never go short, whatever else is in the heap. In a larger circle two ways lead to the answer; where there are more than eight degrees of
// nearness the first is tried first:
// - By searches, from the nearest cell to the farthest: from each, among what is no nearer, for the references to it. What has no inner reference left that holds is taken away. In a list, a tree with links to
//   the parents, a ring, a class that keeps its instances, each search is over after a few cells. In a tangle each goes through all of it.
// - By division. Add the cells from the farthest to the nearest: each inner reference has a time at which its two ends become strongly connected, and it does not hold if that is the time at which v is
//   added. Those times are found for all at once by divide and conquer: the components of the references that are there by the middle of a span of time, with all that was connected before the span taken for
//   one (union-find); the references whose ends they join belong to the earlier half, the others to the later. That looks at each inner reference some three times for each halving of the number of
//   different degrees of nearness in the component, whatever its shape.
//
// The world is stopped meanwhile, so for the larger circles there is a bound: so many looks at cells and inner references in all (FinalizationQuestions::mostLooksAtInnerReferences), for the smallest first, by
// the number of their cells and inner references. One look at each is taken to begin with. A circle for which there are not twice as many left, or not eight for each cell, is left alone, and costs no looks. The
// searches have four looks for each cell and inner reference, or what is left. If there are not enough left for all the halvings of the division, neighbouring degrees of nearness count as one: the rule is
// the same, with a coarser "nearer". That takes references away from what holds, so that still has no circle. With no halving at all, as where it is left alone, the circle holds nothing together, and its
// candidates go in the order in which they were made, when nothing holds any of its cells.
// Which cells are one: in a small circle that takes the third look; in a larger one it is noted where a reference is found not to hold, for the circle in hand (`tiedTo`), and taken over behind the opening.
// As long as the looks suffice nothing depends on the order in which the cells or their references are come to. Behind that, how many looks a search takes depends on the order of a cell's own references, which
// the object fixes, not on where the cells lie: so the order is the same from run to run.
// What that is in time: see README.md. Memory, for the circle in hand: 20 bytes for an inner reference, 68 for a cell, asked for beforehand: if it is not to be had the circle is left alone. For what
// is one: 12 bytes for each cell of the circles that are found from the candidates that something may hold, asked for when the first two are found: if they are not to be had the cells stay two.
Vector<unsigned> DeadGraph::candidatesWithHoldersFirst()
{
    unsigned count = m_cells.size();
    const uint64_t mostLooksAtInnerReferences = m_questions.mostLooksAtInnerReferences;
    auto isLeft = [&](unsigned node) {
        return !(m_flags[node] & Released);
    };

    FastBitVector nothingHolds;
    nothingHolds.resize(count);
    Vector<unsigned> namespacesAndWhatHasName;
    {
        // (Among the living they are not in the graph: see walk().)
        auto namespaces = WTF::move(m_namespacesPassedBy);
        for (PyDict* dict : m_questions.namespaces)
            namespaces.add(dict);
        for (unsigned node = 0; node < count; ++node) {
            auto* function = m_cells[node]->type() == JSFunctionType ? PyFunction::tryFrom(m_cells[node]) : nullptr;
            if (!function)
                continue;
            for (PyDict* dict : { function->globals(), function->builtins() }) {
                if (dict)
                    namespaces.add(dict);
            }
        }
        auto numberOf = [&](JSCell* cell) {
            unsigned* slot = slotFor(cell, false);
            return slot ? *slot : none;
        };
        for (PyDict* dict : namespaces) {
            if (unsigned node = numberOf(dict); node != none) {
                nothingHolds[node] = true;
                namespacesAndWhatHasName.append(node);
            }
            JSValue key;
            JSValue value;
            for (unsigned position = 0; dict->next(position, key, value);) {
                unsigned node = value.isCell() ? numberOf(value.asCell()) : none;
                if (node == none)
                    continue;
                namespacesAndWhatHasName.append(node);
                if (isCandidate(node))
                    nothingHolds[node] = true;
            }
        }
    }

    // A reference counts if its head is another cell, is left, and is not one that nothing holds.
    // The circles: the components of more than one cell, one that others lead to before those. Of each cell, which it is in, and where.
    constexpr unsigned inNoCircle = none - 1;
    struct Circle {
        unsigned firstMember;
        unsigned youngestCandidate; // none: it has no candidate.
        unsigned innerReferences { 0 }; // Only of those that are opened on the account: see below.
        unsigned cellsThatAreHeld { 0 }; // For the last pass, if it holds nothing together.
    };
    Vector<Circle> circles;
    Vector<unsigned> members;
    // `number`: its circle. `low`: its number in that: from when the circle has been opened its place among the members. (`number` stays none for a cell that is never entered: see where the search ends. Nothing that
    // is in the last pass is such a cell.)
    Vector<ComponentSearch::State> places;
    places.fill({ none, none }, count);
    auto circleOf = [&](unsigned node) -> unsigned& {
        return places[node].number;
    };
    auto numberInCircle = [&](unsigned node) -> unsigned& {
        return places[node].low;
    };
    // No reference to these counts, so they are in no circle. To the search they are components that it has found: it asks nothing else about the head of a reference. (Nor does a reference of a cell to
    // itself make a circle.)
    for (unsigned node = 0; node < count; ++node) {
        if (!isLeft(node) || nothingHolds[node])
            circleOf(node) = inNoCircle;
    }
    bool someCircleHasCandidate = false;
    // A candidate, or a cell that lies on a way from a candidate to a candidate, by references that count. Nothing else matters for the order: see the last pass. (What the search does not enter does not have the bit: no
    // reference to it counts.)
    FastBitVector liesBetweenCandidates;
    liesBetweenCandidates.resize(count);
    ComponentSearch search;
    auto findComponentsFrom = [&](auto roots, bool candidatesLeadToThem) {
        forEachComponent(search, roots, places, [&](unsigned node) {
            return m_firstEdge[node];
        }, [&](unsigned node) {
            return m_firstEdge[node + 1];
        }, [&](unsigned, unsigned edge) {
            return m_edges[edge];
        }, [&](std::span<const unsigned> cells) {
            // (What it leads to has been found before it.)
            bool liesBetween = candidatesLeadToThem && std::ranges::any_of(cells, [&](unsigned node) {
                return isCandidate(node) || std::ranges::any_of(successors(node), [&](unsigned target) {
                    return liesBetweenCandidates[target];
                });
            });
            if (liesBetween) {
                for (unsigned node : cells)
                    liesBetweenCandidates[node] = true;
            }
            if (cells.size() == 1) {
                circleOf(cells[0]) = inNoCircle;
                return;
            }
            unsigned youngest = none;
            for (unsigned node : cells) {
                circleOf(node) = circles.size();
                if (isCandidate(node) && (youngest == none || node > youngest))
                    youngest = node;
            }
            someCircleHasCandidate |= youngest != none;
            circles.append({ static_cast<unsigned>(members.size()), youngest, 0, 0 });
            members.append(cells);
        });
    };
    // First from the candidates: what is entered then is what they lead to. They have the lowest numbers, so a search from all the cells begins in the same way: every circle that has a candidate is found here, by that
    // walk, whatever comes behind. (How many looks the opening of a circle takes may depend on the order of its members. So these calls are not to be made one.)
    findComponentsFrom(std::views::iota(0u, m_candidateCount), true);
    // Only a circle that has a candidate is opened, so no cell behind this is ever tied to another. (A circle that holds nothing together needs no places.)
    const size_t placesOfCirclesThatMayBeOpened = members.size();
    // A candidate that nothing holds is not entered, so from what it refers to: in the order of the cells, which is how they lie in memory, not of its references.
    {
        FastBitVector candidateThatNothingHoldsRefersToIt;
        candidateThatNothingHoldsRefersToIt.resize(count);
        for (unsigned candidate = 0; candidate < m_candidateCount; ++candidate) {
            if (!isLeft(candidate) || !nothingHolds[candidate])
                continue;
            for (unsigned target : successors(candidate))
                candidateThatNothingHoldsRefersToIt[target] = true;
        }
        findComponentsFrom(std::views::iota(m_candidateCount, count) | std::views::filter([&](unsigned node) {
            return candidateThatNothingHoldsRefersToIt[node];
        }), true);
    }
    // All else is left out of the last pass. Its circles are wanted for the levels only.
    if (someCircleHasCandidate)
        findComponentsFrom(std::views::iota(m_candidateCount, count), false);
    auto membersOf = [&](unsigned index) {
        unsigned end = index + 1 < circles.size() ? circles[index + 1].firstMember : members.size();
        return members.span().subspan(circles[index].firstMember, end - circles[index].firstMember);
    };
    // (Whether it counts has been asked but for this: what is not left, and what nothing holds, is in no circle.)
    auto isInnerReference = [&](unsigned node, unsigned edge, unsigned circle) {
        return circleOf(m_edges[edge]) == circle && m_edges[edge] != node;
    };
    FastBitVector doesNotHold; // By the number of the reference in m_edges.
    FastBitVector holdsNothingTogether; // By the number of the circle.
    holdsNothingTogether.resize(circles.size());
    for (unsigned index = 0; index < circles.size(); ++index)
        holdsNothingTogether[index] = circles[index].youngestCandidate == none;
    // The cells of a circle that is opened which count as one. By the place in `members`, where what hangs together lies together: which of them stands for it (union-find), the next in a ring of all of them, and,
    // at the one that stands for them, for the last pass, how many of them are held. Made when the first two are found.
    Vector<unsigned> standsFor;
    Vector<unsigned> nextOfThoseThatAreOne;
    Vector<unsigned> thoseThatAreHeld;
    auto whatStandsFor = [](Vector<unsigned>& unionFind, unsigned place) {
        while (unionFind[place] != place)
            place = unionFind[place] = unionFind[unionFind[place]];
        return place;
    };
    auto placeOf = [&](unsigned node) {
        return circles[circleOf(node)].firstMember + numberInCircle(node);
    };

    Vector<unsigned> holders;
    // Hardly any program comes here: what it holds at its end is in circles (the namespaces aside) seldom enough, and a candidate is seldom in one.
    if (someCircleHasCandidate) {
#if ASSERT_ENABLED
        // (The levels go by all the circles: see where the search ends.)
        for (unsigned node = 0; node < count; ++node)
            ASSERT(circleOf(node) != none);
#endif
        doesNotHold.resize(m_edges.size());
        Vector<unsigned> level;
        level.fill(none, count);
        FastBitVector hasSpread;
        hasSpread.resize(count);
        Deque<unsigned> queue;
        auto beginAt = [&](unsigned node) {
            level[node] = 0;
            queue.append(node);
        };
        auto spreadLevels = [&] {
            while (!queue.isEmpty()) {
                unsigned node = queue.takeFirst();
                if (hasSpread[node])
                    continue;
                hasSpread[node] = true;
                unsigned beyond = level[node] + isCandidate(node);
                for (unsigned target : successors(node)) {
                    // (A namespace has level 0 from the start.)
                    if (hasSpread[target] || beyond >= level[target])
                        continue;
                    level[target] = beyond;
                    if (isCandidate(node))
                        queue.append(target);
                    else
                        queue.prepend(target);
                }
            }
        };
        for (unsigned node : namespacesAndWhatHasName)
            beginAt(node);
        // What nothing refers to. (order() has used up m_references.) And by the way the inner references of the circles that are opened on the account, if the looks suffice: see below.
        FastBitVector isInCircleOnAccount;
        isInCircleOnAccount.resize(count);
        for (unsigned index = 0; index < circles.size(); ++index) {
            auto cells = membersOf(index);
            if (circles[index].youngestCandidate == none || cells.size() <= mostCellsOfSmallCircle)
                continue;
            if (8 * static_cast<uint64_t>(cells.size()) > mostLooksAtInnerReferences) {
                holdsNothingTogether[index] = true;
                continue;
            }
            for (unsigned node : cells)
                isInCircleOnAccount[node] = true;
        }
        holders.fill(0, count);
        for (unsigned node = 0; node < count; ++node) {
            for (unsigned target : successors(node))
                holders[target] += target != node;
            if (!isInCircleOnAccount[node])
                continue;
            unsigned innerReferences = 0;
            for (unsigned edge = m_firstEdge[node]; edge < m_firstEdge[node + 1]; ++edge)
                innerReferences += isInnerReference(node, edge, circleOf(node));
            circles[circleOf(node)].innerReferences += innerReferences;
        }
        for (unsigned node = 0; node < count; ++node) {
            if (!holders[node])
                beginAt(node);
        }
        spreadLevels();

        // What no way has led to: from the holders down, where to enter it. (That is in a circle: something refers to it, to which no way has led either.) Then the levels spread from all those places at once.
        {
            FastBitVector liesBehindEntry;
            liesBehindEntry.resize(count);
            Vector<unsigned> entries;
            Vector<unsigned> worklist;
            for (unsigned index = circles.size(); index--;) {
                auto cells = membersOf(index);
                // (As one of its cells, so all.)
                if (level[cells[0]] != none || liesBehindEntry[cells[0]])
                    continue;
                for (unsigned node : cells) {
                    liesBehindEntry[node] = true;
                    worklist.append(node);
                }
                // (Through what is no candidate the level does not rise: without one, any cell does.)
                entries.append(circles[index].youngestCandidate != none ? circles[index].youngestCandidate : cells[0]);
                while (!worklist.isEmpty()) {
                    for (unsigned target : successors(worklist.takeLast())) {
                        if (level[target] != none || liesBehindEntry[target])
                            continue;
                        liesBehindEntry[target] = true;
                        worklist.append(target);
                    }
                }
            }
            for (unsigned node : entries)
                beginAt(node);
            spreadLevels();
        }

        // The lower, the nearer.
        auto nearness = [&](unsigned node) {
            ASSERT(level[node] != none);
            return static_cast<uint64_t>(level[node]) << 33 | (isCandidate(node) ? 1ull << 32 | (none - node) : 0);
        };
        struct Inner {
            // Their numbers in the circle.
            unsigned from;
            unsigned to;
            unsigned edge;
            // When both its ends are there, twice; and 1 if its head is no farther than its tail: no other reference can fail to hold.
            unsigned timeAndWhetherItLeadsNearer;
            unsigned time() const { return timeAndWhetherItLeadsNearer >> 1; }
        };
        struct Span {
            unsigned firstTime;
            unsigned lastTime;
            unsigned begin; // In `inner`.
            unsigned end;
        };
        struct Near {
            uint64_t nearness;
            unsigned cell;
        };
        Vector<Inner> inner;
        Vector<Span> spans;
        Vector<Near> byNearness; // The nearest first.
        // By the number in the circle.
        Vector<unsigned> firstInner;
        Vector<unsigned> time; // At which the cell is added: the farthest at 0.
        Vector<unsigned> parent; // Union-find: what has become strongly connected before the span in hand is one.
        Vector<unsigned> tiedTo; // Union-find: the two ends of a reference that does not hold, if they are equally near.
        // The graph of a step.
        Vector<unsigned> vertices;
        Vector<unsigned> heads;
        Vector<unsigned> firstHead;
        Vector<unsigned> endOfHeads;
        Vector<ComponentSearch::State> states;
        // For the searches.
        Vector<unsigned> referencesLeft; // Inner references to it that hold, for all that is known, from what has not been taken away.
        Vector<unsigned> searchThatCameToIt;
        Vector<unsigned> reached;
        Vector<unsigned> free;

        // The larger circles: the smallest first, by what is charged: one of a few hundred cells is not to go short because of a monster beside it. (Of two alike the one with the younger candidate: that is a
        // matter of the program, not of the order in which the cells were come to.)
        struct CircleToOpen {
            unsigned circle;
            unsigned cellsAndInnerReferences;
        };
        // From each cell what it reaches among what is no nearer, as a set of bits. A reference to it from one of those does not hold.
        auto openSmallCircle = [&](unsigned index) {
            auto cells = membersOf(index);
            unsigned size = cells.size();
            // They are numbered from the nearest to the farthest.
            std::array<Near, mostCellsOfSmallCircle> nearestFirst;
            std::array<uint64_t, mostCellsOfSmallCircle> referredToBy;
            std::array<uint64_t, mostCellsOfSmallCircle> reachedFrom;
            for (unsigned i = 0; i < size; ++i)
                nearestFirst[i] = { nearness(cells[i]), cells[i] };
            std::ranges::sort(std::span { nearestFirst }.first(size), { }, &Near::nearness);
            for (unsigned i = 0; i < size; ++i)
                numberInCircle(nearestFirst[i].cell) = i;
            for (unsigned i = 0; i < size; ++i) {
                unsigned node = nearestFirst[i].cell;
                referredToBy[i] = 0;
                for (unsigned edge = m_firstEdge[node]; edge < m_firstEdge[node + 1]; ++edge) {
                    if (isInnerReference(node, edge, index))
                        referredToBy[i] |= 1ull << numberInCircle(m_edges[edge]);
                }
            }
            uint64_t noNearer = 0;
            for (unsigned i = 0; i < size; ++i) {
                if (!i || nearestFirst[i].nearness != nearestFirst[i - 1].nearness)
                    noNearer = ~((1ull << i) - 1);
                uint64_t reached = 1ull << i;
                for (uint64_t toGoOnFrom = reached; toGoOnFrom;) {
                    uint64_t found = referredToBy[std::countr_zero(toGoOnFrom)] & noNearer & ~reached;
                    toGoOnFrom &= toGoOnFrom - 1;
                    reached |= found;
                    toGoOnFrom |= found;
                }
                reachedFrom[i] = reached;
            }
            for (unsigned i = 0; i < size; ++i) {
                unsigned node = nearestFirst[i].cell;
                for (unsigned edge = m_firstEdge[node]; edge < m_firstEdge[node + 1]; ++edge) {
                    if (isInnerReference(node, edge, index) && ((reachedFrom[numberInCircle(m_edges[edge])] >> i) & 1))
                        doesNotHold[edge] = true;
                }
            }
        };
#if ASSERT_ENABLED
        // Of each of `size` places from `first` on, the first that it is one with, counted from `first`.
        auto partsOf = [&](Vector<unsigned>& unionFind, unsigned first, unsigned size) {
            Vector<unsigned> firstOfPart;
            firstOfPart.fill(none, size);
            Vector<unsigned> parts;
            for (unsigned i = 0; i < size; ++i) {
                unsigned& firstOfIts = firstOfPart[whatStandsFor(unionFind, first + i) - first];
                if (firstOfIts == none)
                    firstOfIts = i;
                parts.append(firstOfIts);
            }
            return parts;
        };
        // Whether the cells of a circle that are one are those that its opening has just tied.
        auto tiesAreTakenOver = [&](unsigned index) {
            unsigned size = membersOf(index).size();
            auto tied = partsOf(tiedTo, 0, size);
            if (standsFor.isEmpty())
                return std::ranges::equal(tied, std::views::iota(0u, size));
            return tied == partsOf(standsFor, circles[index].firstMember, size);
        };
#endif
        // Two cells of a circle that has been opened, by their numbers in it, which are their places among its members. If there is no room for that they stay two: the circle is open all the same, and what holds
        // the one does not hold what hangs off the other.
        bool thereIsRoomForWhatIsOne = true;
        auto makeOne = [&](unsigned index, unsigned i, unsigned j) {
            ASSERT(circles[index].firstMember + membersOf(index).size() <= placesOfCirclesThatMayBeOpened);
            if (standsFor.isEmpty()) {
                size_t size = placesOfCirclesThatMayBeOpened;
                thereIsRoomForWhatIsOne = thereIsRoomForWhatIsOne && standsFor.tryReserveCapacity(size) && nextOfThoseThatAreOne.tryReserveCapacity(size) && thoseThatAreHeld.tryReserveCapacity(size);
                if (!thereIsRoomForWhatIsOne) {
                    standsFor.clear();
                    nextOfThoseThatAreOne.clear();
                    return;
                }
                standsFor.resize(size);
                for (unsigned place = 0; place < size; ++place)
                    standsFor[place] = place;
                nextOfThoseThatAreOne = standsFor;
                thoseThatAreHeld.fill(0, size);
            }
            unsigned a = whatStandsFor(standsFor, circles[index].firstMember + i);
            unsigned b = whatStandsFor(standsFor, circles[index].firstMember + j);
            if (a != b) {
                standsFor[a] = b;
                std::swap(nextOfThoseThatAreOne[a], nextOfThoseThatAreOne[b]); // Of two rings one.
            }
        };
        Vector<CircleToOpen> circlesToOpen;
        for (unsigned index = 0; index < circles.size(); ++index) {
            if (holdsNothingTogether[index])
                continue;
            size_t size = membersOf(index).size();
            if (size <= mostCellsOfSmallCircle)
                continue;
            if (2 * (size + circles[index].innerReferences) > mostLooksAtInnerReferences)
                holdsNothingTogether[index] = true;
            else
                circlesToOpen.append({ index, static_cast<unsigned>(size + circles[index].innerReferences) });
        }
        std::ranges::sort(circlesToOpen, [&](auto& a, auto& b) {
            return a.cellsAndInnerReferences != b.cellsAndInnerReferences ? a.cellsAndInnerReferences < b.cellsAndInnerReferences : circles[a.circle].youngestCandidate > circles[b.circle].youngestCandidate;
        });
        // Room for all that a circle needs, before anything is done: nothing is to fail half way with the world stopped. What is too small goes first: the old and the new are never there together. They come in
        // rising order, so twice as much is asked for, but no more than the largest needs.
        size_t mostCells = 0;
        size_t mostInnerReferences = 0;
        for (auto& [index, cellsAndInnerReferences] : circlesToOpen) {
            mostCells = std::max(mostCells, membersOf(index).size());
            mostInnerReferences = std::max<size_t>(mostInnerReferences, circles[index].innerReferences);
        }
        auto hasRoomFor = [&](size_t cellCount, size_t innerReferenceCount) {
            auto reserve = [](size_t needed, size_t most, auto&... vectors) {
                return ([&] {
                    if (vectors.capacity() >= needed)
                        return true;
                    vectors.clear();
                    return vectors.tryReserveCapacity(std::max(needed, std::min(most, 2 * needed))) || vectors.tryReserveCapacity(needed);
                }() && ...);
            };
            return reserve(innerReferenceCount, mostInnerReferences, inner, heads)
                && reserve(cellCount + 1, mostCells + 1, byNearness, firstInner, time, parent, tiedTo, vertices, firstHead, endOfHeads, states, referencesLeft, searchThatCameToIt, reached, free);
        };
        auto open = [&](unsigned index, unsigned cellsAndInnerReferences, uint64_t& looksLeft) {
            auto cells = membersOf(index);
            unsigned size = cells.size();
            // What follows takes a look at each cell and each inner reference to begin with, and as many again, and more, to find anything out.
            if (looksLeft < 8 * static_cast<uint64_t>(size) || looksLeft < 2 * static_cast<uint64_t>(cellsAndInnerReferences) || !hasRoomFor(size, cellsAndInnerReferences - size)) {
                holdsNothingTogether[index] = true;
                return;
            }
            byNearness.shrink(0);
            tiedTo.resize(size);
            for (unsigned i = 0; i < size; ++i) {
                numberInCircle(cells[i]) = i;
                byNearness.append({ nearness(cells[i]), i });
                tiedTo[i] = i;
            }
            auto tie = [&](unsigned a, unsigned b) {
                a = whatStandsFor(tiedTo, a);
                tiedTo[a] = whatStandsFor(tiedTo, b);
            };
            std::ranges::sort(byNearness, { }, &Near::nearness);
            ASSERT(byNearness[0].nearness != byNearness.last().nearness); // (It has a candidate, and nothing else is as near as a candidate is.)
            time.resize(size);
            unsigned degrees = 1;
            for (unsigned place = size; place--;) {
                degrees += place + 1 < size && byNearness[place].nearness != byNearness[place + 1].nearness;
                time[byNearness[place].cell] = degrees - 1;
            }
            inner.shrink(0);
            firstInner.resize(size + 1);
            for (unsigned i = 0; i < size; ++i) {
                unsigned node = cells[i];
                firstInner[i] = inner.size();
                for (unsigned edge = m_firstEdge[node]; edge < m_firstEdge[node + 1]; ++edge) {
                    if (isInnerReference(node, edge, index))
                        inner.append({ i, numberInCircle(m_edges[edge]), edge, 0 });
                }
            }
            firstInner[size] = inner.size();
            ASSERT(size + inner.size() == cellsAndInnerReferences);
            looksLeft -= cellsAndInnerReferences;

            // First by searches, from the nearest cell to the farthest: from each, among what is no nearer, for the references to it. What has no inner reference left that holds is on no circle any
            // more, and is taken away, with what that frees in turn. In a list, a tree with links to the parents, a ring, each search is over after a few cells. In a tangle each goes through all of it:
            // so they have so many looks and no more. They work out what the division below does.
            auto openBySearches = [&] {
                const uint64_t allowance = std::min<uint64_t>(looksLeft, 4 * static_cast<uint64_t>(cellsAndInnerReferences));
                uint64_t looks = allowance;
                // (What has been found stays: the division finds it again, with a coarser "nearer", too.)
                auto giveUp = [&] {
                    looksLeft -= allowance;
                    return false;
                };
                constexpr unsigned isTakenAway = none;
                referencesLeft.fill(0, size);
                searchThatCameToIt.fill(0, size);
                for (auto& reference : inner)
                    ++referencesLeft[reference.to];
                unsigned numberOfSearch = 0;
                for (unsigned place = 0; place < size;) {
                    unsigned end = place;
                    while (end < size && byNearness[end].nearness == byNearness[place].nearness)
                        ++end;
                    free.shrink(0);
                    for (; place < end; ++place) {
                        unsigned cell = byNearness[place].cell;
                        if (searchThatCameToIt[cell] == isTakenAway)
                            continue;
                        searchThatCameToIt[cell] = ++numberOfSearch;
                        reached.shrink(0);
                        reached.append(cell);
                        for (unsigned next = 0; next < reached.size() && referencesLeft[cell]; ++next) {
                            unsigned from = reached[next];
                            uint64_t cost = 1 + firstInner[from + 1] - firstInner[from];
                            if (looks < cost)
                                return giveUp();
                            looks -= cost;
                            for (unsigned position = firstInner[from]; position < firstInner[from + 1]; ++position) {
                                auto& reference = inner[position];
                                if (reference.to == cell) {
                                    if (!doesNotHold[reference.edge]) {
                                        doesNotHold[reference.edge] = true;
                                        --referencesLeft[cell];
                                        if (time[from] == time[cell])
                                            tie(from, cell);
                                    }
                                    continue;
                                }
                                if (searchThatCameToIt[reference.to] == isTakenAway || searchThatCameToIt[reference.to] == numberOfSearch || time[reference.to] > time[cell])
                                    continue;
                                searchThatCameToIt[reference.to] = numberOfSearch;
                                reached.append(reference.to);
                            }
                        }
                        if (!referencesLeft[cell])
                            free.append(cell);
                    }
                    for (unsigned cell : free)
                        searchThatCameToIt[cell] = isTakenAway;
                    while (!free.isEmpty()) {
                        unsigned from = free.takeLast();
                        for (unsigned position = firstInner[from]; position < firstInner[from + 1]; ++position) {
                            auto& reference = inner[position];
                            if (doesNotHold[reference.edge] || searchThatCameToIt[reference.to] == isTakenAway || --referencesLeft[reference.to])
                                continue;
                            searchThatCameToIt[reference.to] = isTakenAway;
                            free.append(reference.to);
                        }
                    }
                }
                looksLeft -= allowance - looks;
                return true;
            };

            // Then by division. A halving of the number of degrees looks at each inner reference some three times. If there are not enough looks left for all the halvings, neighbouring degrees count as
            // one: the rule is the same, with a coarser "nearer". What holds is less then, so it still has no circle; and with no halving at all the circle holds nothing together.
            auto openByDivision = [&] {
                unsigned halvings = 0;
                while ((degrees - 1) >> halvings)
                    ++halvings;
                uint64_t looksForHalving = 3 * static_cast<uint64_t>(inner.size());
                unsigned degreesInOne = 0; // As a power of two.
                if (looksLeft / looksForHalving < halvings) {
                    degreesInOne = halvings - looksLeft / looksForHalving;
                    halvings -= degreesInOne;
                }
                looksLeft -= halvings * looksForHalving;
                if (!halvings)
                    return false;
                parent.resize(size);
                firstHead.resize(size);
                endOfHeads.resize(size);
                states.fill({ none, none }, size);
                for (unsigned i = 0; i < size; ++i) {
                    time[i] >>= degreesInOne;
                    parent[i] = i;
                }
                for (auto& reference : inner)
                    reference.timeAndWhetherItLeadsNearer = std::max(time[reference.from], time[reference.to]) << 1 | (time[reference.from] <= time[reference.to]);
                auto find = [&](unsigned i) {
                    return whatStandsFor(parent, i);
                };
                spans.append({ 0, (degrees - 1) >> degreesInOne, 0, static_cast<unsigned>(inner.size()) });
                while (!spans.isEmpty()) {
                    Span span = spans.takeLast();
                    auto references = inner.mutableSpan().subspan(span.begin, span.end - span.begin);
                    if (references.empty())
                        continue;
                    if (span.firstTime == span.lastTime) {
                        // That is when their ends become one: for a reference that does not hold, as soon as its head is there.
                        for (auto& reference : references) {
                            if (reference.timeAndWhetherItLeadsNearer == (span.firstTime << 1 | 1)) {
                                doesNotHold[reference.edge] = true;
                                // (A cell stands for itself until the time at which it is added: all its references are of that time or later. What stands for a farther tail is farther.)
                                if (time[reference.from] == span.firstTime)
                                    tie(reference.from, reference.to);
                            }
                            parent[find(reference.from)] = find(reference.to);
                        }
                        continue;
                    }
                    unsigned middle = span.firstTime + (span.lastTime - span.firstTime) / 2;
                    vertices.shrink(0);
                    auto addVertex = [&](unsigned i) {
                        if (states[i].number != none)
                            return;
                        states[i].number = 0;
                        endOfHeads[i] = 0;
                        vertices.append(i);
                    };
                    unsigned headCount = 0;
                    for (auto& reference : references) {
                        // (From here on what stands for its ends is as good as they are.)
                        reference.from = find(reference.from);
                        reference.to = find(reference.to);
                        if (reference.time() > middle)
                            continue;
                        addVertex(reference.from);
                        addVertex(reference.to);
                        ++endOfHeads[reference.from];
                        ++headCount;
                    }
                    heads.resize(headCount);
                    unsigned sum = 0;
                    for (unsigned i : vertices) {
                        states[i].number = none;
                        firstHead[i] = sum;
                        sum += std::exchange(endOfHeads[i], sum);
                    }
                    for (auto& reference : references) {
                        if (reference.time() <= middle)
                            heads[endOfHeads[reference.from]++] = reference.to;
                    }
                    unsigned componentsOfStep = 0;
                    forEachComponent(search, vertices.span(), states, [&](unsigned i) {
                        return firstHead[i];
                    }, [&](unsigned i) {
                        return endOfHeads[i];
                    }, [&](unsigned, unsigned position) {
                        return heads[position];
                    }, [&](std::span<const unsigned> together) {
                        for (unsigned i : together)
                            states[i].number = componentsOfStep;
                        ++componentsOfStep;
                    });
                    auto later = std::ranges::partition(references, [&](const Inner& reference) {
                        return reference.time() <= middle && states[reference.from].number == states[reference.to].number;
                    });
                    for (unsigned i : vertices)
                        states[i].number = none;
                    unsigned split = span.end - later.size();
                    // The earlier half first: the later one counts on what that has joined.
                    spans.append({ middle + 1, span.lastTime, split, span.end });
                    spans.append({ span.firstTime, middle, span.begin, split });
                }
                return true;
            };
#if ASSERT_ENABLED
            // The two ways are to come to the same: in a circle that is not large both are gone beforehand, with all the looks that they want, and what they leave is put back: the circle is then opened
            // as in a build without this, on the same account.
            if (size <= 10000) {
                uint64_t looksLeftInTruth = std::exchange(looksLeft, std::numeric_limits<uint64_t>::max());
                auto takeWhatWasFound = [&] {
                    Vector<unsigned> found;
                    for (auto& reference : inner) {
                        if (doesNotHold[reference.edge])
                            found.append(reference.edge);
                        doesNotHold[reference.edge] = false;
                    }
                    std::ranges::sort(found);
                    return found;
                };
                auto takeWhatWasTied = [&] {
                    auto parts = partsOf(tiedTo, 0, size);
                    for (unsigned i = 0; i < size; ++i)
                        tiedTo[i] = i;
                    return parts;
                };
                if (openBySearches()) {
                    auto foundBySearches = takeWhatWasFound();
                    auto tiedBySearches = takeWhatWasTied();
                    auto innerAsItWas = inner;
                    bool hasOpened = openByDivision();
                    ASSERT_UNUSED(hasOpened, hasOpened);
                    ASSERT(takeWhatWasFound() == foundBySearches);
                    ASSERT(takeWhatWasTied() == tiedBySearches);
                    inner = WTF::move(innerAsItWas);
                }
                // (Searches that have given up leave what they have found by then.)
                takeWhatWasFound();
                takeWhatWasTied();
                looksLeft = looksLeftInTruth;
            }
#endif
            constexpr unsigned fewDegrees = 8;
            if (!(degrees > fewDegrees && openBySearches()) && !openByDivision()) {
                holdsNothingTogether[index] = true;
                return;
            }
#if ASSERT_ENABLED
            // Whichever way it went, and whatever its size: what is tied is what the rule says, by the degrees with which it has been opened.
            Vector<unsigned> tiedByTheRule;
            for (unsigned i = 0; i < size; ++i)
                tiedByTheRule.append(i);
            for (unsigned i = 0; i < size; ++i) {
                for (unsigned edge = m_firstEdge[cells[i]]; edge < m_firstEdge[cells[i] + 1]; ++edge) {
                    if (doesNotHold[edge] && time[numberInCircle(m_edges[edge])] == time[i])
                        tiedByTheRule[whatStandsFor(tiedByTheRule, i)] = whatStandsFor(tiedByTheRule, numberInCircle(m_edges[edge]));
                }
            }
            ASSERT(partsOf(tiedByTheRule, 0, size) == partsOf(tiedTo, 0, size));
#endif
        };
        for (unsigned index = 0; index < circles.size(); ++index) {
            auto cells = membersOf(index);
            if (holdsNothingTogether[index] || cells.size() > mostCellsOfSmallCircle)
                continue;
            openSmallCircle(index);
            unsigned cellsWithoutFinalizer = 0;
            for (unsigned i = 0; i < cells.size(); ++i) {
                numberInCircle(cells[i]) = i;
                cellsWithoutFinalizer += !isCandidate(cells[i]);
            }
            // (Nothing else is as near as a candidate is.)
            for (unsigned i = 0; cellsWithoutFinalizer > 1 && thereIsRoomForWhatIsOne && i < cells.size(); ++i) {
                for (unsigned edge = m_firstEdge[cells[i]]; edge < m_firstEdge[cells[i] + 1]; ++edge) {
                    if (doesNotHold[edge] && nearness(m_edges[edge]) == nearness(cells[i]))
                        makeOne(index, i, numberInCircle(m_edges[edge]));
                }
            }
#if ASSERT_ENABLED
            // The other ways are to come to the same. (So a build with assertions goes all three in every small circle.)
            unsigned innerReferences = 0;
            auto takeWhatWasFound = [&] {
                Vector<unsigned> found;
                innerReferences = 0;
                for (unsigned node : cells) {
                    for (unsigned edge = m_firstEdge[node]; edge < m_firstEdge[node + 1]; ++edge) {
                        innerReferences += isInnerReference(node, edge, index);
                        if (doesNotHold[edge])
                            found.append(edge);
                        doesNotHold[edge] = false;
                    }
                }
                return found;
            };
            auto foundWithSets = takeWhatWasFound();
            for (unsigned i = 0; i < cells.size(); ++i)
                ASSERT(numberInCircle(cells[i]) == i);
            ASSERT(!holdsNothingTogether[index]);
            uint64_t allTheLooksThatItTakes = std::numeric_limits<uint64_t>::max();
            open(index, cells.size() + innerReferences, allTheLooksThatItTakes);
            bool hadRoom = !holdsNothingTogether[index];
            holdsNothingTogether[index] = false;
            auto foundOtherwise = takeWhatWasFound();
            ASSERT(!hadRoom || foundOtherwise == foundWithSets);
            ASSERT(!hadRoom || !thereIsRoomForWhatIsOne || tiesAreTakenOver(index));
            for (unsigned edge : foundWithSets)
                doesNotHold[edge] = true;
#endif
        }
        uint64_t looksLeft = mostLooksAtInnerReferences;
        for (auto& [index, cellsAndInnerReferences] : circlesToOpen) {
            open(index, cellsAndInnerReferences, looksLeft);
            if (holdsNothingTogether[index])
                continue;
            for (unsigned i = 0; thereIsRoomForWhatIsOne && i < membersOf(index).size(); ++i) {
                if (tiedTo[i] != i)
                    makeOne(index, i, tiedTo[i]);
            }
            ASSERT(!thereIsRoomForWhatIsOne || tiesAreTakenOver(index));
        }
    }

    // The last pass. Only what lies between candidates is in it, which in most heaps is next to nothing. A candidate is taken when no other cell is free to go, and is free to go by then if every candidate has gone from
    // which references that hold lead to it. A cell that leads to no candidate lies on no such way. A cell that no candidate leads to waits for none, and nor does what refers to it: that is such a cell itself, or a
    // namespace, or has been released. What holds has no circle, so all of them have gone before the first candidate is taken. (What counts as one lies in one circle, so it is left out as a whole or not at all.)
    auto isLeftOut = [&](unsigned node) {
        return !liesBetweenCandidates[node] && !(isCandidate(node) && isLeft(node) && nothingHolds[node]);
    };
#if ASSERT_ENABLED
    {
        auto wasEntered = [&](unsigned node) {
            return isLeft(node) && !nothingHolds[node];
        };
        FastBitVector candidateLeadsToIt;
        candidateLeadsToIt.resize(count);
        Vector<unsigned> worklist;
        for (unsigned candidate = 0; candidate < m_candidateCount; ++candidate) {
            if (isLeft(candidate))
                worklist.append(candidate);
        }
        while (!worklist.isEmpty()) {
            for (unsigned target : successors(worklist.takeLast())) {
                if (!wasEntered(target) || candidateLeadsToIt[target])
                    continue;
                candidateLeadsToIt[target] = true;
                worklist.append(target);
            }
        }
        for (unsigned node = 0; node < count; ++node) {
            ASSERT(liesBetweenCandidates[node] == (wasEntered(node) && (isCandidate(node) || (candidateLeadsToIt[node] && std::ranges::any_of(successors(node), [&](unsigned target) {
                return target != node && liesBetweenCandidates[target];
            })))));
        }
    }
#endif
    FastBitVector holds; // By the number of the reference in m_edges.
    holds.resize(m_edges.size());
    holders.fill(0, count);
    for (unsigned node = 0; node < count; ++node) {
        if (isLeftOut(node))
            continue;
        unsigned circle = circleOf(node);
        ASSERT(circle != none);
        bool isOpen = circle != inNoCircle && !holdsNothingTogether[circle];
        for (unsigned edge = m_firstEdge[node]; edge < m_firstEdge[node + 1]; ++edge) {
            unsigned target = m_edges[edge];
            // (What has the bit is left, and is not one that nothing holds.)
            if (target == node || !liesBetweenCandidates[target] || (circle != inNoCircle && circle == circleOf(target) && !(isOpen && !doesNotHold[edge])))
                continue;
            ASSERT(!isOpen || circle != circleOf(target) || standsFor.isEmpty() || whatStandsFor(standsFor, placeOf(node)) != whatStandsFor(standsFor, placeOf(target))); // What is one does not hold itself.
            holds[edge] = true;
            ++holders[target];
        }
    }
    // The holders are counted cell by cell. Of the cells that a cell is one with, itself among them: how many are held.
    auto thoseThatAreHeldWith = [&](unsigned node) -> unsigned& {
        unsigned circle = circleOf(node);
        ASSERT(circle < circles.size());
        return holdsNothingTogether[circle] ? circles[circle].cellsThatAreHeld : thoseThatAreHeld[whatStandsFor(standsFor, placeOf(node))];
    };

    Vector<unsigned> withoutFinalizer;
    LowestFirst withFinalizer(m_candidateCount); // The oldest first.
    auto isFreeToGo = [&](unsigned node) {
        if (isCandidate(node))
            withFinalizer.add(node);
        else
            withoutFinalizer.append(node);
    };
    auto allThatAreOneAreFreeToGo = [&](unsigned node) {
        unsigned circle = circleOf(node);
        ASSERT(circle < circles.size());
        if (holdsNothingTogether[circle]) {
            for (unsigned member : membersOf(circle))
                isFreeToGo(member);
            return;
        }
        unsigned first = placeOf(node);
        unsigned place = first;
        do {
            isFreeToGo(members[place]);
            place = nextOfThoseThatAreOne[place];
        } while (place != first);
    };
    // A bit in the count of its holders says that a cell is one with others: so a cell that is not, as nearly all are not, is told by the count, which is in hand, and nothing else that lies scattered is looked at.
    constexpr unsigned isOneWithOthers = 1u << 31;
    ASSERT(m_edges.size() < isOneWithOthers);
    for (unsigned index = 0; index < circles.size(); ++index) {
        auto cells = membersOf(index);
        // (As one of its cells, so all.)
        if (isLeftOut(cells[0]))
            continue;
        if (holdsNothingTogether[index]) {
            for (unsigned node : cells) {
                circles[index].cellsThatAreHeld += !!holders[node];
                holders[node] |= isOneWithOthers;
            }
            if (!circles[index].cellsThatAreHeld)
                allThatAreOneAreFreeToGo(cells[0]);
            continue;
        }
        if (standsFor.isEmpty())
            continue;
        unsigned first = circles[index].firstMember;
        for (unsigned i = 0; i < cells.size(); ++i) {
            if (nextOfThoseThatAreOne[first + i] == first + i)
                continue;
            thoseThatAreHeld[whatStandsFor(standsFor, first + i)] += !!holders[cells[i]];
            holders[cells[i]] |= isOneWithOthers;
        }
        for (unsigned i = 0; i < cells.size(); ++i) {
            if (standsFor[first + i] == first + i && nextOfThoseThatAreOne[first + i] != first + i && !thoseThatAreHeld[first + i])
                allThatAreOneAreFreeToGo(cells[i]);
        }
    }
    for (unsigned node = 0; node < count; ++node) {
        if (!holders[node] && !isLeftOut(node))
            isFreeToGo(node);
    }
    Vector<unsigned> result;
    FastBitVector hasGone;
    hasGone.resize(m_candidateCount);
    while (!withoutFinalizer.isEmpty() || !withFinalizer.isEmpty()) {
        unsigned node;
        if (!withoutFinalizer.isEmpty())
            node = withoutFinalizer.takeLast();
        else {
            node = withFinalizer.takeLowest();
            ASSERT(isLeft(node));
            result.append(node);
            hasGone[node] = true;
        }
        for (unsigned edge = m_firstEdge[node]; edge < m_firstEdge[node + 1]; ++edge) {
            if (!holds[edge])
                continue;
            unsigned holdersLeft = --holders[m_edges[edge]];
            if (!holdersLeft)
                isFreeToGo(m_edges[edge]);
            else if (holdersLeft == isOneWithOthers && !--thoseThatAreHeldWith(m_edges[edge]))
                allThatAreOneAreFreeToGo(m_edges[edge]);
        }
    }
    // What holds has no circle, so nothing is left. What were left would be freed without its finalizer having been called.
    for (unsigned candidate = 0; candidate < m_candidateCount; ++candidate) {
        if (!isLeft(candidate) || hasGone[candidate])
            continue;
        ASSERT_NOT_REACHED();
        result.append(candidate);
    }
    return result;
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

Vector<unsigned> DeadGraph::orderOfTheLiving()
{
    walk();
    m_flags.fill(0, m_cells.size());
    return candidatesWithHoldersFirst();
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
    if (m_questions.wantsAllThatReferenceCountsWouldFree) {
        Vector<unsigned> worklist;
        for (unsigned i = 0; i < m_questions.candidatesInQueues; ++i) {
            if (!(m_flags[i] & Released)) {
                m_flags[i] |= Released;
                worklist.append(i);
            }
        }
        flagAllReachedFrom(WTF::move(worklist), Released);
        for (unsigned node = 0; node < count; ++node) {
            if (m_flags[node] & Released)
                result.allFreedByReferenceCounts.append(m_cells[node]);
        }
        if (m_questions.wantsWhatWaitsForThat) {
            Vector<unsigned> waiting;
            for (unsigned node = 0; node < count; ++node) {
                if (!(m_flags[node] & Released) && rank[node]) {
                    m_flags[node] |= Waits;
                    waiting.append(node);
                }
            }
            flagAllReachedFrom(WTF::move(waiting), Waits);
            for (unsigned i = 0; i < m_candidateCount; ++i) {
                if ((m_flags[i] & (Released | Waits)) == Waits)
                    result.later.append(i);
            }
            for (unsigned candidate : released) {
                if (!rank[candidate])
                    result.now.append(candidate);
            }
        }
        return result;
    }

    // What another candidate reaches waits: it is kept alive by that one now, and comes up again in the collection that finds
    // that one gone, unless its finalizer found it a home. That takes a collection per level. A structure that is deeper than a
    // few levels (a linked list of objects with finalizers) is finalized in one round. So is all that dies when the modules have gone: what has waited a round stands in the lists in the order that this
    // round has given it, not in the order of making, by which candidatesWithHoldersFirst() tells the younger of two.
    constexpr unsigned deepestToTakeLevelByLevel = 3;
    bool takesAll = deepest > deepestToTakeLevelByLevel || m_questions.holdersComeFirst;
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
    auto take = [&](unsigned candidate) {
        if (m_flags[candidate] & Released)
            return;
        if (m_flags[candidate] & Waits)
            result.later.append(candidate);
        else
            result.now.append(candidate);
    };
    if (m_questions.holdersComeFirst && m_candidateCount - released.size() > 1) {
        for (unsigned candidate : candidatesWithHoldersFirst())
            take(candidate);
    } else {
        for (unsigned i = 0; i < m_candidateCount; ++i)
            take(i);
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
        // Not by way of what the engine shares among them. The Structure of the instances of a class that have the same attributes is held weakly: it dies with the last of them, and all the dead
        // of that shape refer to it.
        auto joins = [&](unsigned node) {
            return !(m_flags[node] & Released) && isCellOfTheProgram(m_cells[node]);
        };
        for (unsigned node = 0; node < count; ++node) {
            if (!joins(node))
                continue;
            for (unsigned successor : successors(node)) {
                // The lower number stands for the piece: that of its oldest candidate.
                if (unsigned a = pieceOf(node), b = pieceOf(successor); joins(successor) && a != b)
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
    if (candidates.size() < 2 && !questions.wantsGarbage && !questions.wantsToKnowWhatReferenceCountsWouldFree && !questions.wantsAllThatReferenceCountsWouldFree && questions.askedAbout.empty()) {
        FinalizationOrder result;
        if (!candidates.empty())
            result.now.append(0);
        result.freedByReferenceCounts = result.now.size();
        return result;
    }
    return DeadGraph(heap, marker, candidates, questions).order();
}

Vector<unsigned> orderWithHoldersFirst(JSC::Heap& heap, std::span<JSCell* const> candidates, std::span<PyDict* const> namespaces, uint64_t mostLooksAtInnerReferences)
{
    return DeadGraph(heap, candidates, { .namespaces = namespaces, .mostLooksAtInnerReferences = mostLooksAtInnerReferences }).orderOfTheLiving();
}

} // namespace Python
} // namespace JSC
