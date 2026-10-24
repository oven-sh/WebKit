/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTValueFlowGraph.h"
#include "AOTValueFlowLog.h"
#include "AOTValueFlowRuleContext.h"
#include "AOTVector.h"
#include <array>
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/EnumSet.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/MonotonicTime.h>
#include <wtf/Seconds.h>
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace AOT {

class BuildContext;
struct Subject;

namespace Typed {

class ValueFlowInstancedFunctions;

using ValueFlowRule = void (*)(const ValueFlowStatement&, ValueFlowRuleContext&);

// What the bodies give. Body i is function i; the last body is the host's, which is reached from the start.
struct ValueFlowSolverInput {
    // For the rules alone.
    const Program* program { nullptr };
    const ValueFlowUniverse* universe { nullptr };
    const ValueFlowProgramNodes* nodes { nullptr };
    // Takes the counts and the diagnostics of the solve. Without it the solve reports nothing.
    BuildContext* build { nullptr };

    // The node that holds whatever has been handed to code that nobody compiles: the unknown object stands for all of that. With it the solver makes one statement of kind Escape, which is due from the start.
    ValueFlowNodeID escaped;
    // The node that holds whatever the analysis has stopped following: the untracked object stands for all of that. No statement runs for it. Without it no set is ever given up.
    ValueFlowNodeID lost;
    ValueFlowPlaceholders placeholders;

    uint32_t numberOfNodes { 0 };
    std::span<const ValueFlowEdge> edges;
    std::span<const ValueFlowNodeMask> masks;
    std::span<const ValueFlowStatement> statements;
    std::span<const ValueFlowOperand> operands;
    std::span<const ValueFlowEdge> edgesOnceReached;
    std::span<const ValueFlowNodeSet> seedsOnceReached;
    std::span<const FunctionID> functionsEnteredOnceReached;
    // One more than there are bodies.
    std::span<const ValueFlowBodyRange> ranges;

    // A set of more layouts and callees than this is given up: its members go to `lost`, and the untracked object stands for them.
    uint64_t mostSetMembers { UINT64_MAX };
    // A rule that looks at the members of a value one by one takes a value with more members than this as a whole: a method call passes all its receivers to every callee.
    uint64_t mostMembersToldApart { UINT64_MAX };
    // Once the rules have logged more edges than this, every set that still moves is given up. Where no set can be given up the solve stops without a result.
    uint64_t mostEdgesLogged { UINT64_MAX };
    // For each edge that a rule has logged the solver remembers the statement that logged it first. It costs 20 bytes an edge: for tests and reports.
    bool remembersWhoLoggedEachEdge { false };
    // Somebody asks unknownSourceOf() of a node, explain(), or the graph for the predecessors of a node once the solve is over. It costs a walk over all that holds the unknown object and a copy of all edges, and only reports and tests ask.
    bool isAskedHowValuesCame { true };
    // For tests that look at how a solve goes: it stops after this many rounds, and what it holds then is no result.
    uint32_t mostRounds { UINT32_MAX };
    // For tests that ask what a program would be like if the table described more: a rule that names one of these sources gets the primitives alone, and its edges keep the name of the source.
    EnumSet<ValueFlowUnknownSource> unknownSourcesLeftOut;
    // The build excludes evaluated text: the run time refuses eval, Function and its relatives every text, so a call of one throws and runs nothing.
    bool refusesEvaluatedText { false };
    // CompileOptions::removedFeatures.
    std::array<uint64_t, 8> removedFeatures { };
    // The functions whose calls the rules tell apart. Null where there are none.
    const ValueFlowInstancedFunctions* instancedFunctions { nullptr };
};

// Why a function is reached: the statement that reached it, or the body that enters it as soon as that body is reached itself. Neither for the host's body.
struct ValueFlowReachWitness {
    ValueFlowStatementID statement;
    uint32_t enteringBody { UINT32_MAX };
    // The number of rounds that were finished when the function was reached.
    uint32_t round { 0 };
};

// Pairs of numbers, each kept once.
template<typename T>
class ListWithRoomToFill {
    WTF_MAKE_NONCOPYABLE(ListWithRoomToFill);
    static_assert(std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>);
public:
    ListWithRoomToFill() = default;
    ListWithRoomToFill(ListWithRoomToFill&& other)
        : m_elements(WTF::move(other.m_elements))
        , m_size(std::exchange(other.m_size, 0))
    {
    }

    ListWithRoomToFill& operator=(ListWithRoomToFill&& other)
    {
        m_elements = WTF::move(other.m_elements);
        m_size = std::exchange(other.m_size, 0);
        return *this;
    }

    size_t size() const { return m_size; }
    bool isEmpty() const { return !m_size; }
    size_t capacity() const { return m_elements.span().size(); }
    std::span<const T> span() const LIFETIME_BOUND { return m_elements.span().first(m_size); }
    std::span<T> mutableSpan() LIFETIME_BOUND { return m_elements.mutableSpan().first(m_size); }
    const T& operator[](size_t index) const LIFETIME_BOUND { return span()[index]; }
    T& operator[](size_t index) LIFETIME_BOUND { return mutableSpan()[index]; }
    auto begin() const LIFETIME_BOUND { return span().begin(); }
    auto end() const LIFETIME_BOUND { return span().end(); }
    auto begin() LIFETIME_BOUND { return mutableSpan().begin(); }
    auto end() LIFETIME_BOUND { return mutableSpan().end(); }

    void reserveCapacity(size_t size)
    {
        if (size > capacity())
            m_elements.realloc(size * sizeof(T));
    }

    void makeRoomToFill(size_t more)
    {
        if (!more)
            return;
        if (m_size + more > capacity())
            m_elements.realloc(std::max<size_t>({ 1024, capacity() * 2, m_size + more }) * sizeof(T));
        m_size += more;
    }

    void append(const T& element)
    {
        makeRoomToFill(1);
        mutableSpan()[m_size - 1] = element;
    }

    void shrink(size_t size)
    {
        ASSERT(size <= m_size);
        m_size = size;
    }

    void clear()
    {
        m_elements = { };
        m_size = 0;
    }

private:
    MallocSpan<T> m_elements;
    size_t m_size { 0 };
};

// A list that threads add to while others read what is there. It never moves: the addresses for all it may ever hold are set aside at once, and memory comes with the pages that are written.
template<typename T>
class ListThatStaysInPlace {
    WTF_MAKE_NONCOPYABLE(ListThatStaysInPlace);
    static_assert(std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>);
public:
    static constexpr size_t mostElements = 1u << 27;

    ListThatStaysInPlace() = default;
    ~ListThatStaysInPlace();

    size_t size() const { return m_size.load(std::memory_order_relaxed); }
    std::span<const T> span() const LIFETIME_BOUND { return unsafeMakeSpan(m_elements, size()); }
    const T& operator[](size_t index) const LIFETIME_BOUND
    {
        ASSERT(index < size());
        return m_elements[index];
    }
    T& operator[](size_t index) LIFETIME_BOUND
    {
        ASSERT(index < size());
        return m_elements[index];
    }
    auto begin() const LIFETIME_BOUND { return span().begin(); }
    auto end() const LIFETIME_BOUND { return span().end(); }
    size_t capacity() const { return m_numberWithMemory.load(std::memory_order_relaxed); }

    // From any thread. Answers where the room begins; it holds nothing, and whoever has asked for it writes every element of it before anybody else hears of it.
    size_t makeRoomToFill(size_t more)
    {
        size_t first = m_size.fetch_add(more, std::memory_order_relaxed);
        if (first + more > m_numberWithMemory.load(std::memory_order_acquire)) [[unlikely]]
            takeMemoryFor(first + more);
        return first;
    }

private:
    void takeMemoryFor(size_t);

    T* m_elements { nullptr };
    std::atomic<size_t> m_numberWithMemory { 0 };
    Lock m_lock;
    // Every thread that adds writes this, and every thread that reads needs the line above.
    alignas(64) std::atomic<size_t> m_size { 0 };
};

// Bits that the threads of a loop set and clear, whichever word they lie in.
class ValueFlowSharedBits {
public:
    size_t size() const { return m_words.size() * 32; }
    void growTo(size_t size)
    {
        if (size > this->size())
            m_words.insertFill(m_words.size(), 0u, (size + 31) / 32 - m_words.size());
    }
    bool get(size_t index) const { return WTF::atomicLoad(const_cast<uint32_t*>(&m_words[index >> 5]), std::memory_order_relaxed) >> (index & 31) & 1; }
    // Both answer what the bit was.
    bool set(size_t index) { return WTF::atomicExchangeOr(&m_words[index >> 5], 1u << (index & 31), std::memory_order_relaxed) >> (index & 31) & 1; }
    bool clear(size_t index) { return WTF::atomicExchangeAnd(&m_words[index >> 5], ~(1u << (index & 31)), std::memory_order_relaxed) >> (index & 31) & 1; }
    void clearAll() { zeroSpan(m_words.mutableSpan()); }

private:
    Vector<uint32_t> m_words;
};

class ValueFlowPairSet {
public:
    bool add(uint32_t first, uint32_t second);
    bool contains(uint32_t first, uint32_t second) const;
    // Ahead of add(), so that the slot is there when it is looked at.
    void prefetch(uint32_t first, uint32_t second) const;
    size_t memoryInBytes() const { return m_slots.sizeInBytes(); }

private:
    void grow();

    // One more than (first << 32 | second); zero for a free slot.
    MallocSpan<uint64_t> m_slots;
    size_t m_size { 0 };
};

// The rounds.
class ValueFlowSolver {
    WTF_MAKE_NONCOPYABLE(ValueFlowSolver);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ValueFlowSolver);
public:
    struct Counters {
        uint32_t rounds { 0 };
        uint32_t fullRounds { 0 };
        uint64_t statementRuns { 0 };
        uint64_t edgesLogged { 0 };
        uint64_t testsWhetherSetsMeet { 0 };
        uint32_t functionsReached { 0 };
        uint32_t madeNodesAtRest { 0 };
        uint32_t nodesSaturated { 0 };
        uint32_t nodesHoldingObjectsOutsideFamily { 0 };
        bool isPastEdgeBudget { false };
        Seconds rules;
        Seconds merge;
        Seconds values;
    };

    // What one round has done.
    struct Round {
        bool isFull { false };
        uint32_t turns { 0 };
        uint32_t statementRuns { 0 };
        uint32_t functionsReached { 0 };
        uint32_t nodesMade { 0 };
        uint32_t edgesAdded { 0 };
        uint32_t nodesMoved { 0 };
        uint32_t waves { 0 };
        uint32_t evaluations { 0 };
    };

    struct RunsAndEdges {
        uint64_t runs { 0 };
        // Runs that asked for no edge, node or function that was not there: they need not have been due.
        uint64_t runsThatAddedNothing { 0 };
        // Runs in a round of every statement that did add something: they should have been due before.
        uint64_t runsThatWereNotDueAndAdded { 0 };
        // The edges asked for, and those among them that no earlier round had added.
        uint64_t connections { 0 };
        uint64_t edgesLogged { 0 };
    };

    enum class Rounds : uint8_t {
        // A statement runs when it is due.
        Due,
        // The same without the round at the end in which every statement runs once more. That round is a quarter to a third of all runs, and on no program has it added anything.
        DueWithoutCheck,
        // Every statement runs in every round. The results are the same.
        Full,
    };

    ValueFlowSolver(const ValueFlowSolverInput&, ValueSets&, ValueFlowRule, const ParallelLoops&, Rounds = Rounds::Due);
    ~ValueFlowSolver();

    // False if the solve has stopped at a limit: then nothing that it holds is a result. That happens only where the input names no node for what is given up.
    bool solve();

    const ValueSets& sets() const { return m_sets; }
    const ValueFlowGraph& graph() const { return m_graph; }
    std::span<const std::unique_ptr<ValueSetMaker>> makersIdleAfterSolve() const LIFETIME_BOUND { return m_makers.span(); }
    // As the graph holds it: beside a placeholder it may or may not list what the placeholder covers.
    ValueSetID valueOf(ValueFlowNodeID node) const { return m_graph.valueOf(node); }
    bool isReached(FunctionID function) const { return m_isReached.quickGet(function.index()); }
    OptionSet<BuiltinUse> usesOf(BuiltinID) const;
    // After the solve. What was done through the untracked object counts as done to every object that the analysis has stopped following.
    OptionSet<ObjectMutation> marksOf(ValueFlowObjectID) const;

    // After the solve. The type of the node was taken on trust, and an object that the analysis follows has arrived there that lies outside it: nothing is proven about the node.
    bool holdsObjectOutsideFamily(ValueFlowNodeID node) const { return m_holdsObjectOutsideFamily.get(node.index()); }

    // After a solve with Rounds::Due. The runs that added something in a round of every statement: each should have been due before. The solve has gone on from there, so what it holds is at rest all the same.
    uint64_t numberOfRunsThatWereNotDueAndAdded() const { return m_numberOfRunsThatWereNotDueAndAdded; }
    // The first of them, by the number of the statement within each round.
    std::span<const ValueFlowStatementID> firstStatementsThatWereNotDueAndAdded() const LIFETIME_BOUND { return m_firstStatementsThatWereNotDueAndAdded.span(); }
    // After a solve with Rounds::Due.
    struct AddedByGeneralRules {
        bool isAnything() const { return nodesThatHoldMore || functionsReached || builtinsUsedMore; }

        uint64_t nodesThatHoldMore { 0 };
        uint32_t functionsReached { 0 };
        uint32_t builtinsUsedMore { 0 };
        Vector<ValueFlowNodeID> firstNodesThatHoldMore;
    };
    const AddedByGeneralRules& addedByGeneralRules() const LIFETIME_BOUND { return m_addedByGeneralRules; }

    const ValueFlowSolverInput& input() const LIFETIME_BOUND { return m_input; }
    uint32_t numberOfStatements() const { return m_input.statements.size() + m_madeStatements.size(); }
    const ValueFlowStatement& statementAt(ValueFlowStatementID statement) const LIFETIME_BOUND
    {
        size_t index = statement.index();
        return index < m_input.statements.size() ? m_input.statements[index] : m_madeStatements[index - m_input.statements.size()].statement;
    }

    // Among the statements that reached the function in the turn in which it was reached: a statement of a body before one that a rule has made, and the one with the lowest number, or content.
    ValueFlowReachWitness reachedBy(FunctionID function) const { return m_reachedBy[function.index()]; }
    // How the member has come into the node: the node first, then a predecessor that holds the member, and so on, on a shortest way, to a node that holds it for ever. After the solve only where the input has asked for it.
    Vector<ValueFlowNodeID> explain(ValueFlowNodeID, ValueFlowObjectID member) const;
    // The closures that come to the callee of a construction by ways that pass through nothing made for the unknown object.
    ValueSetID closuresFollowedToConstruction(ValueFlowNodeID callee) const
    {
        auto found = m_closuresFollowedToConstruction.find(callee.index());
        return found == m_closuresFollowedToConstruction.end() ? ValueSets::nothing : found->value;
    }
    // None for an edge of a body, for one that the solver has made itself (from what is written to what is read, into a masked node, between groups of callees) and where the input did not ask for it.
    ValueFlowStatementID statementThatLogged(ValueFlowEdge) const;
    struct LoggedEdge {
        ValueFlowEdge edge;
        ValueFlowStatementID statement;
        uint32_t round { 0 };
    };
    // The same edges in the order of the rounds in which they were logged first, and by their numbers within a round.
    std::span<const LoggedEdge> edgesInOrderLogged() const LIFETIME_BOUND { return m_edgesInOrderLogged.span(); }
    // Of an edge along which a rule has brought the unknown object into being: what the rule has named as its source.
    std::optional<ValueFlowUnknownSource> unknownSourceOf(ValueFlowEdge) const;
    // After the solve, of a node that holds the unknown object: the source at the far end of a shortest way on which it has come. None where it has come from the seeds of bodies alone, and where the input did not ask for it.
    std::optional<ValueFlowUnknownSource> unknownSourceOf(ValueFlowNodeID) const;
    // None for a node of a body.
    const ValueFlowNodeKey* keyOfMadeNode(ValueFlowNodeID) const LIFETIME_BOUND;

    // Operations by opcode, the other statements by kind.
    void forEachKindOfStatementRun(NOESCAPE const Invocable<void(ValueFlowStatementKind, Opcode, const RunsAndEdges&)> auto& functor) const
    {
        for (unsigned index = 0; index < m_runsAndEdges.size(); ++index) {
            if (!m_runsAndEdges[index].runs)
                continue;
            if (index < numberOfOpcodes)
                functor(ValueFlowStatementKind::Operation, static_cast<Opcode>(index), m_runsAndEdges[index]);
            else
                functor(static_cast<ValueFlowStatementKind>(index - numberOfOpcodes), Opcode { }, m_runsAndEdges[index]);
        }
    }

    // Of a statement that a rule has made: what it was made for. The one statement of kind Escape has no key.
    bool isMadeByRule(ValueFlowStatementID statement) const { return statement.index() >= m_input.statements.size(); }
    const ValueFlowNodeKey& keyOf(ValueFlowStatementID statement) const LIFETIME_BOUND { return m_madeStatements[statement.index() - m_input.statements.size()].key; }
    // Nobody asks for what it was made for any more: whoever did asks for that of a bigger set, which covers it.
    bool isAtRest(ValueFlowStatementID) const;
    // What the last run of the statement got from read(), written(), lookup(), group(), hub() and masked(), each once, with the statement that was made with it, if any.
    void forEachMadeNodeAskedForBy(ValueFlowStatementID statement, NOESCAPE const Invocable<void(const ValueFlowNodeKey&, ValueFlowStatementID)> auto& functor) const
    {
        if (statement.index() >= m_firstUseOfStatement.size())
            return;
        for (uint32_t use = m_firstUseOfStatement[statement.index()]; use != UINT32_MAX; use = m_uses[use].next) {
            const MadeNodes& madeNodes = m_madeNodes[m_uses[use].madeNodes];
            functor(madeNodes.key, madeNodes.statement);
        }
    }

    void forEachMadeNodeAskedForBy(ValueFlowStatementID statement, NOESCAPE const Invocable<void(const ValueFlowNodeKey&, ValueFlowStatementID, ValueFlowNodeID first)> auto& functor) const
    {
        if (statement.index() >= m_firstUseOfStatement.size())
            return;
        for (uint32_t use = m_firstUseOfStatement[statement.index()]; use != UINT32_MAX; use = m_uses[use].next) {
            const MadeNodes& madeNodes = m_madeNodes[m_uses[use].madeNodes];
            functor(madeNodes.key, madeNodes.statement, madeNodes.first);
        }
    }

    // The same for the statements from `first` up to `end`, in their order. The lists of several statements are followed side by side: each step along a list waits for memory.
    void forEachMadeNodeAskedForByStatements(uint32_t first, uint32_t end, NOESCAPE const Invocable<void(uint32_t statement)> auto& beginStatement, NOESCAPE const Invocable<void(uint32_t indexOfMadeNodes, const ValueFlowNodeKey&, ValueFlowStatementID)> auto& functor) const
    {
        constexpr uint32_t listsSideBySide = 16;
        uint32_t endOfLists = std::min<uint32_t>(end, m_firstUseOfStatement.size());
        std::array<uint32_t, listsSideBySide> use;
        std::array<Vector<uint32_t, 32>, listsSideBySide> found;
        for (uint32_t base = first; base < end; base += listsSideBySide) {
            uint32_t count = std::min(listsSideBySide, end - base);
            uint32_t numberUnfinished = 0;
            for (uint32_t list = 0; list < count; ++list) {
                found[list].shrink(0);
                use[list] = base + list < endOfLists ? m_firstUseOfStatement[base + list] : UINT32_MAX;
                if (use[list] == UINT32_MAX)
                    continue;
                ++numberUnfinished;
                __builtin_prefetch(&m_uses[use[list]]);
            }
            while (numberUnfinished) {
                for (uint32_t list = 0; list < count; ++list) {
                    if (use[list] == UINT32_MAX)
                        continue;
                    const Use& current = m_uses[use[list]];
                    found[list].append(current.madeNodes);
                    __builtin_prefetch(&m_madeNodes[current.madeNodes]);
                    use[list] = current.next;
                    if (use[list] == UINT32_MAX)
                        --numberUnfinished;
                    else
                        __builtin_prefetch(&m_uses[use[list]]);
                }
            }
            for (uint32_t list = 0; list < count; ++list) {
                beginStatement(base + list);
                for (uint32_t index : found[list])
                    functor(index, m_madeNodes[index].key, m_madeNodes[index].statement);
            }
        }
    }

    // A key that forEachMadeNodeAskedForBy() has handed out, as a number.
    uint32_t indexOfMadeNodes(const ValueFlowNodeKey& key) const { return static_cast<uint32_t>(reinterpret_cast<const MadeNodes*>(&key) - m_madeNodes.span().data()); }
    const ValueFlowNodeKey& keyOfMadeNodesAt(uint32_t index) const LIFETIME_BOUND { return m_madeNodes[index].key; }
    ValueFlowNodeID firstOfMadeNodesAt(uint32_t index) const { return m_madeNodes[index].first; }
    ValueFlowStatementID statementOfMadeNodesAt(uint32_t index) const { return m_madeNodes[index].statement; }
    uint32_t numberOfRecordsOfMadeNodes() const { return m_madeNodes.size(); }

    const Counters& counters() const { return m_counters; }
    std::span<const Round> rounds() const LIFETIME_BOUND { return m_roundsDone.span(); }
    // At the end of the rounds.
    size_t memoryInBytes() const;

private:
    friend class ValueFlowRuleContext;

    struct MadeNodes {
        ValueFlowNodeKey key;
        ValueFlowNodeID first;
        // The statement that was made with them, if any.
        ValueFlowStatementID statement;
        // Nothing comes out of what is written through no object or handed to no callee.
        bool staysAtRest { false };
        bool hasBeenAtRest { false };
        // Of a node that reads or writes: whether it stands in the lists of its name.
        bool isListedUnderName { false };
        bool isListedUnderMembers { false };
        // It is not linked itself: it takes from, or hands on to, its parts, which are.
        bool isUnionOfParts { false };
        // A rule of the turn that runs has made it.
        bool isOfThisTurn { false };
        bool hasStatementRunWhereMade { false };
        // The group of a big set of callees has no statement: it hands on to the groups of the parts of the set, which the groups of other sets share.
        uint32_t firstPart { 0 };
        uint32_t numberOfParts { 0 };
        // While somebody asks for it, it asks for its parts.
        uint32_t firstUseOfParts { UINT32_MAX };
    };

    static constexpr uint32_t everyAsker = UINT32_MAX - 1;
    bool keepsAwake(ValueFlowStatementID asker, uint32_t madeNodes) const
    {
        uint32_t askerThatKeepsNothingAwake = m_askerThatKeepsNothingAwake[madeNodes];
        return askerThatKeepsNothingAwake != everyAsker && askerThatKeepsNothingAwake != asker.index();
    }

    struct MadeStatement {
        ValueFlowStatement statement;
        ValueFlowNodeKey key;
    };

    struct Use {
        uint32_t madeNodes { 0 };
        uint32_t next { UINT32_MAX };
    };

    struct Reader {
        ValueFlowStatementID statement;
        uint32_t next { UINT32_MAX };
    };

    struct NodeOfSet {
        ValueSetID set;
        ValueFlowNodeID node;
    };

    // The nodes that read, or that write, under one name.
    struct NodesOfName {
        Vector<NodeOfSet> ofFewObjects;
        // Listed by their members too, in chains of their own: a node of few objects finds them there, a node of many looks at each of them once.
        Vector<NodeOfSet> ofManyObjects;
        // Found only by a look at each: the sets with very many members and the complements.
        Vector<NodeOfSet> ofVeryManyObjects;
    };

    struct Name {
        ValueFlowName name;
        NodesOfName read;
        NodesOfName written;
    };

    // The nodes that stand for every name of one kind.
    struct KindOfNames {
        NodesOfName read;
        NodesOfName written;
        // Bit by kind.
        uint64_t readKindsThatMayFindIt { 0 };
        uint64_t writtenKindsItMayFind { 0 };
    };
    static constexpr unsigned mostKindsOfNames = 64;
    static constexpr uint32_t numberOfFirstKindAmongNames = 1u << 30;

    // A placeholder and the node that holds what it stands for.
    struct Covering {
        ValueFlowObjectID placeholder;
        ValueFlowNodeID covered;
        ValueSetID objectsThatMayBeCovered { ValueSets::nothing };
        // All but the objects in that node: what a rule sees of a set that holds the placeholder. Primitives and keys are never cut. Computed once after each solve of the graph.
        ValueSetID allButCovered { ValueSets::anything };
    };

    struct KindThatCoversKeys {
        ValueFlowObjectID kind;
        ValueSetID allButKeys;
    };

    struct ObjectOutsideFamily {
        ValueFlowNodeID node;
        // The one with the lowest number.
        ValueFlowObjectID object;
    };
    String describeWayOf(const ObjectOutsideFamily&) const;

    struct Thread;

    // What a rule sees.
    std::optional<ValueFlowNodeID> findMadeNode(const ValueFlowNodeKey&) const;
    // The same for every run of a turn, whatever the other threads have made meanwhile.
    std::optional<ValueFlowNodeID> findNodeMadeBeforeThisTurn(const ValueFlowNodeKey&) const;
    // None for a key that waits for the end of the turn: see makeNodesWhereAskedFor().
    std::optional<uint32_t> findOrMakeNodesWhereAskedFor(const ValueFlowNodeKey&, uint32_t hash, ValueFlowRoundLog&, ValueSetMaker&) const;
    std::span<const ValueFlowOperand> operandsOf(const ValueFlowStatement& statement) const LIFETIME_BOUND { return m_input.operands.subspan(statement.firstOperand, statement.operandCount); }
    bool isKnownWatcher(ValueFlowNodeID node, ValueSetID within, ValueFlowStatementID statement) const
    {
        if (ValueFlowRoundLog::isNumberedByLog(node))
            return false;
        const WatchShard& shard = m_watchShards[watchShardOf(node, within)];
        auto watch = shard.watchOfPart.find(static_cast<uint64_t>(node.index()) << 32 | within.index());
        return watch != shard.watchOfPart.end() && shard.knownWatchers.contains(watch->value, statement.index());
    }
    bool isKnownToWaitForSomethingIn(ValueFlowNodeID node, ValueFlowStatementID statement) const { return !ValueFlowRoundLog::isNumberedByLog(node) && m_readerShards[readerShardOf(node)].knownToWaitForSomething.contains(node.index(), statement.index()); }
    bool hasStatementOfBodyNeverRun(ValueFlowStatementID statement) const
    {
        if (statement.index() >= m_input.statements.size())
            return false;
        return !m_hasStatementOfBodyRun.get(statement.index()) || (m_statementsWait && m_nextStatementWaiting[statement.index()] == hasWaitedAndNotRun);
    }
    void prefetchWhetherKnownReader(ValueFlowNodeID node, ValueFlowStatementID statement) const
    {
        if (!ValueFlowRoundLog::isNumberedByLog(node))
            m_readerShards[readerShardOf(node)].known.prefetch(node.index(), statement.index());
    }
    void prefetchWhetherKnownToWaitForSomethingIn(ValueFlowNodeID node, ValueFlowStatementID statement) const
    {
        if (!ValueFlowRoundLog::isNumberedByLog(node))
            m_readerShards[readerShardOf(node)].knownToWaitForSomething.prefetch(node.index(), statement.index());
    }
    bool isKnownReader(ValueFlowNodeID node, ValueFlowStatementID statement) const { return !ValueFlowRoundLog::isNumberedByLog(node) && m_readerShards[readerShardOf(node)].known.contains(node.index(), statement.index()); }
    // Of a node's value, in the normal form: a set that holds a placeholder lists no object that the placeholder covers, and one that holds the kind of strings or symbols lists no key of that kind.
    ValueSetID valueForReadersOf(ValueFlowNodeID, ValueSetMaker&) const;
    // All that the node held when the last run of the statement asked what was new in it. Nothing for a statement that has not asked, or that has been at rest since.
    ValueSetID valueShownTo(ValueFlowStatementID, ValueFlowNodeID) const;
    // What takes the place of a placeholder at a mask that was made from the type of an operand: the objects of the program that the mask admits. None where a placeholder that the mask admits passes as itself.
    ValueSetID familyObjectsOf(ValueSetID mask, ValueSetMaker&) const;

    // The number of a statement that a rule has made hangs on the threads. This does not: where the order or a choice among statements shows, it goes by this.
    uint64_t contentOf(ValueFlowStatementID) const;
    void makeDue(ValueFlowStatementID);
    void listIfItMayBecomeDue(ValueFlowStatementID, Vector<ValueFlowStatementID>&) const;
    void makeStatementsDue(uint32_t first, uint32_t end);
    void makeEveryStatementDue();
    bool reach(uint32_t body, ValueFlowReachWitness);
    void takeReachedFunctions();
    void countRunsAndEdges();
    void rememberWhoLoggedTheEdges();
    bool runRules();
    void letSomeDueStatementsWait();
    void runDueStatements(bool isFirstLapOfTurn);
    bool merge();
    void numberMadeNodes();
    struct KeyToMake {
        ValueFlowNodeKey key;
        uint64_t hashOfMembers { 0 };
        // What makeNodesOf() answers.
        uint32_t madeNodes { UINT32_MAX };
    };
    class NodeMaker;
    std::optional<uint32_t> findNumberOfMadeNodes(const ValueFlowNodeKey&) const;
    // In the order of their keys, each once, none of them there.
    void makeNodesOf(std::span<KeyToMake>);
    void takeWhatWasMade(std::span<const std::unique_ptr<NodeMaker>>, std::span<KeyToMake>);
    // While the rules of a turn run, the thread that is the first to ask for a key makes its nodes: it takes the next numbers, writes the record and lists it under the key, so every thread has the same numbers from then on.
    std::optional<uint32_t> makeNodesWhereAskedFor(const ValueFlowNodeKey&, uint32_t hash, ValueFlowRoundLog&, ValueSetMaker&) const;
    void letRulesMakeNodes();
    void takeWhatRulesHaveMade();
    static ValueFlowStatement statementMadeWith(const MadeNodes&);
    void runStatementsMadeBy(Thread&);
    uint64_t contentOf(const ValueFlowNodeKey&) const;
    // Behind the last round: the numbers that the threads still hold become nodes that hold nothing, so that every number stands for something.
    void fillNumbersThatNobodyGave();
    ValueFlowNodeID makeNodes(const ValueFlowNodeKey&);
    ValueFlowNodeID ensureConstant(ValueSetID);
    void listToLink(const MadeNodes&);
    void prepareNodesToLink();
    void linkNamedNodes(unsigned shard, bool isRead);
    void takeEdgesOfLinkShards();
    Name& ensureName(ValueFlowName);
    struct Linked;
    struct NodeToLink;
    void link(const NodeOfSet&, bool isRead, NodesOfName& others, uint32_t numberOfName, Linked&);
    void insert(const NodeToLink&, bool isRead, NodesOfName&);
    void linkNode(const NodeToLink&, bool isRead, Linked&);
    void insertNode(const NodeToLink&, bool isRead);
    void removeNodesAtRestFrom(Vector<NodeOfSet>&);
    void linkInChunks(bool isRead, unsigned numberOfOtherTasks, NOESCAPE const ScopedLambda<void(unsigned task, unsigned worker)>& otherTask);
    void noteReaders(unsigned shard, ValueSetMaker&);
    void noteWatchers(unsigned numberOfShard);
    size_t numberOfReaders() const;
    void releaseRoundState();
    void noteFunctionsAsked();
    void noteNodesAskedFor();
    // Records of uses for the threads of a loop: each takes the positions that are its own.
    // The uses that one thread of a loop gives out: it takes a block at a time, of the free ones while there are any.
    struct UsesOfThread {
        size_t nextFree { 0 };
        size_t endOfFree { 0 };
        size_t nextFresh { 0 };
        size_t endOfFresh { 0 };
    };
    uint32_t takeUse(UsesOfThread&);
    void letThreadsTakeUses();
    void takeBackUsesOfThreads();
    Vector<UsesOfThread> m_usesOfThreads;
    std::atomic<size_t> m_numberOfFreeUsesLeft { 0 };
    template<typename T> void growAndFill(ListWithRoomToFill<T>&, size_t size, T value);
    void takeUpPartsOfNodesWithFirstUser(Vector<uint32_t>&& madeNodes);
    void letGoOf(Vector<uint32_t>&& listsOfUses);
    void makeReadersDue(std::span<const ValueFlowNodeID> moved);
    struct WatchShard;
    void watch(WatchShard&, ValueFlowNodeID, ValueSetID within, ValueFlowStatementID);
    void chainByMember(WatchShard&, uint32_t watchedNode, uint32_t watch);
    void chainSetsWatchedForCover();
    struct NewMemberOfWatchedNode {
        // Of the escaped or the lost objects: the number of shards, and the node's number.
        uint32_t shard { 0 };
        uint32_t watchedNode { 0 };
        ValueFlowObjectID member;
    };
    struct WatchesToTest {
        uint32_t shard { 0 };
        uint32_t watchedNode { 0 };
        ValueSetID whatIsNew;
    };
    void listWatchersOf(ValueFlowNodeID, ValueSetMaker&, Vector<ValueFlowStatementID>& readers, Vector<NewMemberOfWatchedNode>&, Vector<WatchesToTest>&);
    // Of makeReadersDue(), empty between two calls.
    Vector<Vector<ValueFlowStatementID>> m_readersOfWorker;
    Vector<Vector<NewMemberOfWatchedNode>> m_newMembersOfWorker;
    Vector<Vector<WatchesToTest>> m_watchesToTestOfWorker;
    Vector<NewMemberOfWatchedNode> m_newMembers;
    void listReadersOfWatch(const WatchShard&, uint32_t watch, Vector<ValueFlowStatementID>& readers) const;
    ValueSetID withoutWhatOtherMembersCover(ValueFlowNodeID, ValueSetMaker&) const;
    ValueSetID withoutWhatOtherMembersCover(ValueSetID, ValueSetMaker&) const;
    ValueSetID findFamilyObjectsOf(ValueSetID mask, ValueSetMaker&) const;
    void noteWhatPlaceholdersCover();
    void connectWhatStoppedPlaceholdersCover();
    void prepareSaturation();
    bool maySaturate(ValueFlowNodeID) const;
    bool isMadeForUnknownObject(ValueFlowNodeID) const;
    bool hasTooManyLayoutsAndCallees(ValueSetID);
    void saturate(std::span<const ValueFlowNodeID> moved);
    void spreadMarks();
    void findObjectsOutsideFamilies();
    ValueSetID findClosuresFollowedTo(ValueFlowNodeID);
    void findClosuresFollowedToConstructions();
    void findUnknownSources(NOESCAPE const ScopedLambda<void()>& whatRunsBesideTheWalk);
    Subject subjectOf(ValueFlowNodeID) const;
    void report();

    const ValueFlowSolverInput& m_input;
    ValueSets& m_sets;
    const ValueFlowRule m_rule;
    const ParallelLoops& m_loops;
    const Rounds m_rounds;

    Vector<std::unique_ptr<ValueSetMaker>> m_makers;
    ValueFlowGraph m_graph;
    Vector<std::unique_ptr<Thread>> m_threads;

    mutable ListThatStaysInPlace<MadeNodes> m_madeNodes;
    // While the rules of a turn run: the numbers that the next block of nodes and the next statement get.
    alignas(64) mutable std::atomic<uint32_t> m_nextNode { 0 };
    mutable std::atomic<uint32_t> m_nextStatement { 0 };
    // Without the numbers that threads hold and have not given.
    alignas(64) uint32_t m_numberOfNodesMade { 0 };
    // Two lists beside the records, for whoever looks at nothing else of them.
    ListWithRoomToFill<uint32_t> m_numberOfUsers;
    // That a statement asks for what it was made with keeps nothing awake. Nor does anybody's asking for what stays at rest.
    ListWithRoomToFill<uint32_t> m_askerThatKeepsNothingAwake;
    // All threads add to these at once, so none shares a cache line with another or with anything that they read meanwhile.
    struct alignas(64) IndexOfMadeNodes {
        // The hash in the upper half, one more than the number of the record in the lower. Zero for a free slot.
        using Slot = std::atomic<uint64_t>;

        // The slots lie at a cache line, so the low bits of their address hold the binary logarithm of their number.
        class Table {
        public:
            Table()
                : m_bits(0)
            {
            }

            Table(Slot* slots, unsigned bits)
                : m_bits(reinterpret_cast<uintptr_t>(slots) | bits)
            {
            }

            explicit operator bool() const { return m_bits; }
            Slot* slots() const { return reinterpret_cast<Slot*>(m_bits & ~static_cast<uintptr_t>(63)); }
            size_t numberOfSlots() const { return static_cast<size_t>(1) << (m_bits & 63); }

        private:
            uintptr_t m_bits;
        };

        ~IndexOfMadeNodes();

        // Under the lock.
        void add(std::atomic<Table>& current, uint32_t hash, uint32_t number, size_t fewestSlots);
        // While nobody looks.
        void freeOutgrownTables();

        Lock lock;
        size_t size { 0 };
        // The last one is the current one.
        Vector<Table, 2> tables;
    };
    // By the ten highest bits of the hash of the key: many, so that two threads seldom meet at one lock. A table begins with room for what a solve of this input usually makes: nobody waits while one grows.
    static constexpr unsigned numberOfIndicesOfMadeNodes = 1024;
    IndexOfMadeNodes& indexOfMadeNodesWith(uint32_t hash) const LIFETIME_BOUND { return m_madeNodesByKey[hash >> 22]; }
    std::atomic<IndexOfMadeNodes::Table>& tableOfMadeNodesWith(uint32_t hash) const LIFETIME_BOUND { return m_tablesOfMadeNodes[hash >> 22]; }
    ALWAYS_INLINE std::optional<uint32_t> findInIndexOfMadeNodes(uint32_t hash, NOESCAPE const Invocable<bool(uint32_t number)> auto& isEqual) const
    {
        IndexOfMadeNodes::Table table = tableOfMadeNodesWith(hash).load(std::memory_order_acquire);
        if (!table)
            return std::nullopt;
        const IndexOfMadeNodes::Slot* slots = table.slots();
        size_t mask = table.numberOfSlots() - 1;
        WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
        for (size_t index = hash & mask;; index = (index + 1) & mask) {
            uint64_t slot = slots[index].load(std::memory_order_acquire);
            if (!slot)
                return std::nullopt;
            if (slot >> 32 == hash && isEqual(static_cast<uint32_t>(slot) - 1))
                return static_cast<uint32_t>(slot) - 1;
        }
        WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
    }
    // Everybody reads these and only a table that grows writes one: they share no line with the locks.
    alignas(64) mutable std::array<std::atomic<IndexOfMadeNodes::Table>, numberOfIndicesOfMadeNodes> m_tablesOfMadeNodes { };
    mutable std::array<IndexOfMadeNodes, numberOfIndicesOfMadeNodes> m_madeNodesByKey;
    size_t m_fewestSlotsOfIndexOfMadeNodes { 64 };
    ListWithRoomToFill<MadeStatement> m_madeStatements;
    // By the number of a made node less the number of nodes of the bodies.
    ListWithRoomToFill<uint32_t> m_madeNodesOfNode;
    Vector<uint32_t> m_partsOfMadeNodes;
    using LatestNodes = std::array<uint32_t, 4>;
    // The nodes of many objects that read or write under some of the names. A name is always with the same one.
    struct NodesOfManyObjects {
        // What the two tables hold is a position in here.
        Vector<uint32_t> madeNodes;
        // By kind and name: the last that were made, the newest first.
        HashMap<uint64_t, LatestNodes, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> latest;
        // By a hash of kind, name and block.
        HashMap<uint64_t, uint32_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> latestWithBlock;
        // By set: ValueSets::sampleOfBlocks(), which walks every block. One set is asked for under many names.
        HashMap<uint32_t, std::array<uint64_t, 4>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> sampleOfSet;
    };
    static constexpr unsigned numberOfNodeMakers = 64;
    std::array<NodesOfManyObjects, numberOfNodeMakers> m_nodesOfManyObjects;
    Vector<uint32_t> m_madeNodesOfStatement;
    ListWithRoomToFill<uint32_t> m_firstUseOfStatement;
    ListThatStaysInPlace<Use> m_uses;
    Vector<uint32_t> m_freeUses;
    uint32_t m_numberOfNodesBeforeRound { 0 };
    uint32_t m_turnsOfRound { 0 };

    // For the phase log alone. A step ends where the next one begins, so the steps add up to the rounds.
    struct StepTimes {
        struct Step {
            ASCIILiteral name;
            Seconds wall;
            Seconds cpuOfThisThread;
        };

        void begin();
        void didFinish(ASCIILiteral step);
        void dump() const;

        bool isOn { false };
        MonotonicTime startOfStep;
        Seconds cpuAtStartOfStep;
        Vector<Step> steps;
        // Of the loop over the due statements, summed over the turns.
        Seconds busyOnAllThreads;
        Seconds busiestThreadOfTurn;
        Seconds longestShareOfTurn;
    };
    StepTimes m_stepTimes;

    BitVector m_isReached;
    // Whoever runs a statement notes it here, in m_isDue and in m_hasRunInTurn.
    ValueFlowSharedBits m_hasStatementOfBodyRun;
    uint64_t m_numberOfTurn { 0 };
    // A read of a property, a call or a construction takes its first operand before it does anything, so while no value has arrived there all that its rule says is that it may throw.
    ValueFlowNodeID nodeAwaitedBy(ValueFlowStatementID, const ValueFlowStatement&) const;
    static constexpr uint32_t doesNotWait = UINT32_MAX;
    static constexpr uint32_t hasWaitedAndNotRun = UINT32_MAX - 1;
    bool m_statementsWait { false };
    // By node of the bodies: one more than the number of the first statement that waits for it. By statement of the bodies: one more than the number of the next one in the list.
    ListWithRoomToFill<uint32_t> m_firstStatementWaitingForNode;
    ListWithRoomToFill<uint32_t> m_nextStatementWaiting;
    Vector<ValueFlowReachWitness> m_reachedBy;
    Vector<RunsAndEdges> m_runsAndEdges;
    uint64_t m_numberOfRunsThatWereNotDueAndAdded { 0 };
    bool m_usesGeneralRulesOnly { false };
    AddedByGeneralRules m_addedByGeneralRules;
    Vector<ValueFlowStatementID> m_firstStatementsThatWereNotDueAndAdded;
    HashMap<uint64_t, ValueFlowStatementID, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_statementOfEdge;
    Vector<LoggedEdge> m_edgesInOrderLogged;
    Vector<uint32_t> m_bodiesReachedInRound;
    size_t m_numberOfBodiesWhoseStatementsAreDue { 0 };

    mutable ValueFlowSharedBits m_isDue;
    // The statements whose last run has asked for very many edges.
    BitVector m_isDear;
    Vector<ValueFlowStatementID> m_dearStatements;
    Vector<ValueFlowStatementID> m_due;
    // For tests (TestingSwitch::DelayFlowRules, DelayOtherFlowRules): each seed is another valid schedule. The programs must answer the same under each, with about the same code.
    uint32_t m_seedOfSchedule { 0 };
    Vector<ValueFlowStatementID> m_dueStatementsThatWait;
    ValueFlowSharedBits m_hasRunInTurn;
    Vector<ValueFlowStatementID> m_dueInNextTurn;

    ListWithRoomToFill<uint32_t> m_firstReaderOfNode;
    ListWithRoomToFill<ValueSetID> m_seenByReadersOfNode;
    // A byte a node, so that each has one writer.
    ListWithRoomToFill<uint8_t> m_isWaitedForToHoldSomething;
    bool m_isFullRound { false };
    struct NodeRead {
        ValueFlowNodeID node;
        ValueFlowStatementID statement;
    };
    // The readers of a node are noted in the shard of the node, so that the shards are filled side by side.
    struct ReaderShard {
        Vector<Reader> readers;
        ValueFlowPairSet known;
        // The statements that wait for an empty node to hold something, in `readers` too. An entry stays when its node has told them.
        ValueFlowPairSet knownToWaitForSomething;
        HashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> firstReaderWaitingForSomething;
    };
    static constexpr unsigned numberOfReaderShards = 64;
    static constexpr unsigned readerShardOf(ValueFlowNodeID node) { return node.index() / 16 % numberOfReaderShards; }
    std::array<ReaderShard, numberOfReaderShards> m_readerShards;
    // The statements that wait for a node to hold more of a set. They are found by what is new in the node, member by member.
    struct Watch {
        ValueSetID within;
        uint32_t firstReader { UINT32_MAX };
        uint32_t watchedNode { 0 };
    };
    struct WatchedNode {
        ValueFlowNodeID node;
        // All that the node held when its watchers were last looked for. Not of the escaped and the lost objects, which have several shards.
        ValueSetID seen;
        Vector<uint32_t> watches;
        Vector<uint32_t> watchesOfManyObjects;
        // The node of the escaped or of the lost objects.
        bool holdsWhatPlaceholderCovers { false };
        // Until a node has many watches each of them is asked whether what is new meets its set: they stand among watchesOfManyObjects.
        bool hasChainsByMember { false };
    };
    struct ChainedWatch {
        uint32_t watch { 0 };
        uint32_t next { UINT32_MAX };
    };
    struct MemberOfSetWatchedForCover {
        uint32_t member { 0 };
        uint32_t set { 0 };
    };
    static constexpr unsigned numberOfRangesOfMembers = 16;
    static constexpr unsigned rangeOfMember(uint32_t member) { return member / 1024 % numberOfRangesOfMembers; }
    // By node, as the readers: a word of m_isWatched belongs to one shard. Most watches are at the escaped and the lost objects: those have shards of their own, by the set watched.
    struct WatchShard {
        Vector<Reader> readers;
        Vector<Watch> watches;
        HashMap<uint64_t, uint32_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> watchOfPart;
        ValueFlowPairSet knownWatchers;
        Vector<WatchedNode> watchedNodes;
        HashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> watchedNodeOfNode;
        HashMap<uint64_t, uint32_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> firstWatchOfMember;
        Vector<ChainedWatch> watchesOfMembers;
        // The same sets are watched at the node of the escaped objects and at that of the lost ones, and most watches are theirs: such a set is listed once for both and names its watches, one a node.
        HashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> numberOfSetWatchedForCover;
        Vector<Vector<uint32_t, 2>> watchesOfSetWatchedForCover;
        // The members of the sets that are new in this round, by range, for chainSetsWatchedForCover().
        std::array<Vector<MemberOfSetWatchedForCover>, numberOfRangesOfMembers> membersToChain;
        uint32_t endOfMembersToChain { 0 };
    };
    static constexpr unsigned numberOfWatchShardsByNode = 8;
    static constexpr unsigned numberOfWatchShardsBySet = 16;
    static constexpr unsigned numberOfWatchShards = numberOfWatchShardsByNode + numberOfWatchShardsBySet;
    // `escaped`, `lost` and the two marks that rules ask after: nearly every statement that stands on an object watches them, each for its own few objects. So their watches are kept by the set watched.
    bool holdsObjectsWithRareWrite(ValueFlowNodeID node) const { return m_firstOfObjectsWithRareWrite && node && node.index() - m_firstOfObjectsWithRareWrite.index() < numberOfValueFlowRareWrites; }
    bool holdsWhatPlaceholderCovers(ValueFlowNodeID node) const { return node == m_input.escaped || node == m_input.lost || (node && (node == m_objectsThatAreNotPlain || node == m_objectsWrittenUnderUnknownKey)) || holdsObjectsWithRareWrite(node); }
    ValueSetID& seenByWatchersOf(ValueFlowNodeID node) LIFETIME_BOUND
    {
        if (holdsObjectsWithRareWrite(node))
            return m_seenByWatchersOfObjectsWithRareWrite[node.index() - m_firstOfObjectsWithRareWrite.index()];
        return node == m_input.escaped ? m_seenByWatchersOfEscaped : node == m_input.lost ? m_seenByWatchersOfLost : node == m_objectsThatAreNotPlain ? m_seenByWatchersOfObjectsThatAreNotPlain : m_seenByWatchersOfObjectsWrittenUnderUnknownKey;
    }
    ValueFlowNodeID m_firstOfObjectsWithRareWrite;
    std::array<ValueSetID, numberOfValueFlowRareWrites> m_seenByWatchersOfObjectsWithRareWrite { ValueSets::nothing, ValueSets::nothing, ValueSets::nothing, ValueSets::nothing };
    ValueFlowNodeID m_objectsThatAreNotPlain;
    ValueFlowNodeID m_objectsWrittenUnderUnknownKey;
    ValueSetID m_seenByWatchersOfObjectsThatAreNotPlain { ValueSets::nothing };
    ValueSetID m_seenByWatchersOfObjectsWrittenUnderUnknownKey { ValueSets::nothing };
    unsigned watchShardOf(ValueFlowNodeID node, ValueSetID within) const
    {
        if (holdsWhatPlaceholderCovers(node))
            return numberOfWatchShardsByNode + (within.index() * 0x9E3779B1u >> 28);
        return node.index() / 64 % numberOfWatchShardsByNode;
    }
    std::array<WatchShard, numberOfWatchShards> m_watchShards;
    // The sets watched at the escaped and the lost objects.
    Vector<uint32_t> m_firstSetWatchedForCoverOfMember;
    std::array<Vector<ChainedWatch>, numberOfRangesOfMembers> m_setsWatchedForCoverOfMembers;
    ValueSetID m_seenByWatchersOfEscaped;
    ValueSetID m_seenByWatchersOfLost;
    BitVector m_isWatched;
    HashMap<uint32_t, Vector<ValueFlowStatementID>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_readersOfFunction;
    // By statement: the nodes of which it is shown only what is new, each with all that it held at the last run.
    HashMap<uint32_t, Vector<ValueFlowNodeSet, 1>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_valuesShownToStatement;
    mutable ListWithRoomToFill<ValueSetID> m_rememberedByStatement;

    Vector<Covering, 2> m_coverings;
    bool m_haveLostObjectsEscaped { false };
    // Neither a placeholder nor a primitive nor a key.
    ValueSetID m_objectsThatPlaceholdersMayCover { ValueSets::nothing };
    // The built-ins that let in unseen code.
    ValueSetID m_objectsAlwaysFollowed { ValueSets::nothing };
    ValueSetID m_objectsLookedForBesideUnknownObject { ValueSets::nothing };
    // Those of them of which something says what they are.
    ValueSetID m_objectsSaidToBeSomething { ValueSets::nothing };
    // What the program allocates with a layout of its own, and its closures.
    ValueSetID m_objectsOfProgram { ValueSets::nothing };
    Vector<KindThatCoversKeys, 2> m_kindsThatCoverKeys;
    // By the mask of an operand of a body.
    HashMap<uint32_t, ValueSetID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_familyObjectsOfMask;

    bool m_saturates { false };
    ValueSetID m_untrackedObjectAlone;
    // By member: zero for what does not count, else a number that members of one layout or one built-in class share.
    ListWithRoomToFill<uint32_t> m_layoutOrCalleeOfMember;
    ListWithRoomToFill<uint32_t> m_lastCountThatSaw;
    uint32_t m_numberOfCounts { 0 };
    HashMap<uint32_t, bool, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_isTooLarge;
    BitVector m_isSaturated;

    Vector<Name> m_names;
    std::array<KindOfNames, mostKindsOfNames> m_kindsOfNames;
    HashMap<uint64_t, uint32_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_numberOfName;
    // By (number of the name * 2 + whether it reads, object): the nodes of few objects that hold the object, as a chain.
    struct ChainedNode {
        ValueFlowNodeID node;
        uint32_t next { UINT32_MAX };
    };
    // The nodes of a name are linked in the shard of the name, so that the shards are linked side by side.
    static constexpr unsigned numberOfLinkShardsOfNames = 64;
    static constexpr unsigned numberOfLinkShardsOfKinds = 8;
    static constexpr unsigned numberOfLinkShards = numberOfLinkShardsOfNames + numberOfLinkShardsOfKinds;
    static constexpr unsigned linkShardOf(uint32_t numberOfName)
    {
        if (numberOfName >= numberOfFirstKindAmongNames)
            return numberOfLinkShardsOfNames + (numberOfName - numberOfFirstKindAmongNames) % numberOfLinkShardsOfKinds;
        return numberOfName % numberOfLinkShardsOfNames;
    }
    // Which of the three lists of a name a node stands in.
    enum class SizeOfSetToLink : uint8_t {
        Few,
        Many,
        VeryMany,
    };
    struct NodesOfMembers {
        HashMap<uint64_t, uint32_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> first;
        Vector<ChainedNode> chained;
    };
    struct Linked {
        Vector<ValueFlowEdge> edges;
        uint64_t testsWhetherSetsMeet { 0 };
        Vector<Vector<NodeOfSet>*> listsMostlyAtRest;
    };
    struct LinkShard {
        // Of the nodes that write and of those that read. While the nodes of one side are linked, only the table of that side takes entries, and only the other is looked at.
        std::array<NodesOfMembers, 2> nodesOfMembers;
        Linked linked;
    };
    std::array<LinkShard, numberOfLinkShards> m_linkShards;
    // Made or woken in this round.
    struct NodeToLink {
        NodeOfSet node;
        ValueFlowName name;
        bool standsForKind { false };
        // The rest is filled in by prepareNodesToLink().
        SizeOfSetToLink size { SizeOfSetToLink::Few };
        bool isDearToLink { false };
        uint32_t numberOfName { 0 };
        uint32_t madeNodes { 0 };
        ValueFlowObjectID onlyMember;
    };
    ListWithRoomToFill<NodeToLink> m_readToLink;
    ListWithRoomToFill<NodeToLink> m_writtenToLink;

    Vector<ValueFlowEdge> m_edgesOfRound;

    Vector<ValueFlowEdge> m_edgesInFrontOfLinks;
    Vector<ValueFlowGraph::StoppedPlaceholder> m_placeholdersStopped;
    Vector<OptionSet<ObjectMutation>> m_marksOfObject;
    Vector<OptionSet<BuiltinUse>> m_usesOfBuiltin;
    BitVector m_statementsWithMembersNotToldApart;
    // In the order of the nodes.
    Vector<ObjectOutsideFamily> m_objectsOutsideFamilies;
    HashMap<uint32_t, ValueSetID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_closuresFollowedToConstruction;
    BitVector m_holdsObjectOutsideFamily;
    // By node: zero for none, else one more than the source.
    Vector<uint8_t> m_unknownSourceOfNode;

    Counters m_counters;
    Vector<Round> m_roundsDone;
    std::optional<size_t> m_memoryInBytesAtEndOfRounds;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
