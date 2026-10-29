/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBackgroundTask.h"
#include "AOTThreads.h"
#include "AOTThreshold.h"
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

struct ValueFlowSolverInput {
    const Program* program { nullptr };
    const ValueFlowUniverse* universe { nullptr };
    const ValueFlowProgramNodes* nodes { nullptr };
    BuildContext* build { nullptr };

    ValueFlowNodeID escaped;
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
    std::span<const ValueFlowBodyRange> ranges;

    uint64_t mostSetMembers { UINT64_MAX };
    uint64_t mostMembersToldApart { UINT64_MAX };
    uint64_t mostEdgesLogged { UINT64_MAX };
    bool remembersWhoLoggedEachEdge { false };
    bool isAskedHowValuesCame { true };
    bool leavesOutWhatIsCovered { false };
    uint32_t mostRounds { UINT32_MAX };
    // With a seed, nodes of the bodies that are reached are given more than the program brings them: whatIsGivenTo() says which and what.
    std::optional<uint32_t> seedOfWidening;
    EnumSet<ValueFlowUnknownSource> unknownSourcesLeftOut;
    RefusedCode refusedCode;
    std::array<uint64_t, 8> removedFeatures { };
    const ValueFlowInstancedFunctions* instancedFunctions { nullptr };
};

struct ValueFlowReachWitness {
    ValueFlowStatementID statement;
    uint32_t enteringBody { UINT32_MAX };
    uint32_t round { 0 };
};

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

    void shrinkCapacity(size_t size)
    {
        ASSERT(size >= m_size);
        if (size >= capacity())
            return;
        if (size)
            m_elements.realloc(size * sizeof(T));
        else
            m_elements = { };
    }

private:
    MallocSpan<T> m_elements;
    size_t m_size { 0 };
};

template<typename T>
class ListThatStaysInPlace {
    WTF_MAKE_NONCOPYABLE(ListThatStaysInPlace);
    static_assert(std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>);
public:
    // Records are named by 32-bit indexes, and the highest indexes have meanings of their own.
    static constexpr size_t mostElements = (static_cast<size_t>(1) << 32) - (1u << 16);

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
    size_t m_numberReserved { 0 };
    std::atomic<size_t> m_numberWithMemory { 0 };
    Lock m_lock;
    alignas(64) std::atomic<size_t> m_size { 0 };
};

class ValueFlowSharedBits {
public:
    size_t size() const { return m_words.size() * 32; }
    void growTo(size_t size)
    {
        if (size > this->size())
            m_words.insertFill(m_words.size(), 0u, (size + 31) / 32 - m_words.size());
    }
    bool get(size_t index) const { return WTF::atomicLoad(const_cast<uint32_t*>(&m_words[index >> 5]), std::memory_order_relaxed) >> (index & 31) & 1; }
    bool set(size_t index) { return WTF::atomicExchangeOr(&m_words[index >> 5], 1u << (index & 31), std::memory_order_relaxed) >> (index & 31) & 1; }
    bool clear(size_t index) { return WTF::atomicExchangeAnd(&m_words[index >> 5], ~(1u << (index & 31)), std::memory_order_relaxed) >> (index & 31) & 1; }
    bool setInOrder(size_t index) { return WTF::atomicExchangeOr(&m_words[index >> 5], 1u << (index & 31), std::memory_order_seq_cst) >> (index & 31) & 1; }
    bool clearInOrder(size_t index) { return WTF::atomicExchangeAnd(&m_words[index >> 5], ~(1u << (index & 31)), std::memory_order_seq_cst) >> (index & 31) & 1; }
    void clearAll() { zeroSpan(m_words.mutableSpan()); }

private:
    Vector<uint32_t> m_words;
};

class ValueFlowPairSet {
public:
    bool add(uint32_t first, uint32_t second);
    bool contains(uint32_t first, uint32_t second) const;
    void prefetch(uint32_t first, uint32_t second) const;
    size_t bytesInUse() const { return m_size * sizeof(uint64_t); }
    size_t memoryInBytes() const { return m_slots.sizeInBytes(); }

private:
    void grow();

    MallocSpan<uint64_t> m_slots;
    size_t m_size { 0 };
};

// From 64-bit keys, none of them all ones, to 32-bit numbers. A slot takes twelve bytes.
class ValueFlowNumbersByKey {
public:
    struct Added {
        uint32_t& number;
        bool isNew;
    };

    Added add(uint64_t key, uint32_t number);
    const uint32_t* find(uint64_t key) const
    {
        if (!m_slots)
            return nullptr;
        size_t mask = m_slots.span().size() - 1;
        for (size_t position = firstPositionOf(key + 1, mask);; position = (position + 1) & mask) {
            const Slot& slot = m_slots[position];
            if (slot.keyPlusOne() == key + 1)
                return &slot.number;
            if (!slot.keyPlusOne())
                return nullptr;
        }
    }
    size_t bytesInUse() const { return m_size * sizeof(Slot); }
    size_t memoryInBytes() const { return m_slots.sizeInBytes(); }
    // The most slots that a search for an absent key looks at.
    size_t longestRunOfSlotsInUse() const;

private:
    struct Slot {
        uint64_t keyPlusOne() const { return static_cast<uint64_t>(highHalfOfKeyPlusOne) << 32 | lowHalfOfKeyPlusOne; }

        uint32_t lowHalfOfKeyPlusOne;
        uint32_t highHalfOfKeyPlusOne;
        uint32_t number;
    };

    // The keys of one table may have been picked by the high bits of one product.
    static size_t firstPositionOf(uint64_t keyPlusOne, size_t mask)
    {
        uint64_t hash = keyPlusOne * 0x9e3779b97f4a7c15ull;
        return (hash ^ hash >> 32) * 0xbf58476d1ce4e5b9ull >> std::countl_zero(mask);
    }
    void grow();

    MallocSpan<Slot> m_slots;
    size_t m_size { 0 };
};

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
        uint64_t runsThatAddedNothing { 0 };
        uint64_t runsThatWereNotDueAndAdded { 0 };
        uint64_t connections { 0 };
        uint64_t edgesLogged { 0 };
    };

    enum class Rounds : uint8_t {
        Due,
        DueWithoutCheck,
        Full,
    };

    ValueFlowSolver(const ValueFlowSolverInput&, ValueSets&, ValueFlowRule, const ParallelLoops&, Rounds = Rounds::Due);
    ~ValueFlowSolver();

    bool solve();

    const ValueSets& sets() const { return m_sets; }
    const ValueFlowGraph& graph() const { return m_graph; }
    std::span<const std::unique_ptr<ValueSetMaker>> makersIdleAfterSolve() const LIFETIME_BOUND { return m_makers.span(); }
    // Everything that may reach the node. A proof reads this. A member that another member covers is left out.
    ValueSetID possibleValuesOf(ValueFlowNodeID node) const { return m_graph.valueOf(node); }
    // What the program itself brings to the node. A prediction reads this: it may name too few, so nothing is proven from it.
    ValueSetID seenValuesOf(ValueFlowNodeID node) const
    {
        if (size_t share = node.index() / nodesPerShareOfListed; share < m_listedWithCoveredMembers.size()) {
            const Vector<ValueFlowNodeSet>& listed = m_listedWithCoveredMembers[share];
            const ValueFlowNodeSet* found = std::ranges::lower_bound(listed, node, { }, &ValueFlowNodeSet::node);
            if (found != listed.end() && found->node == node)
                return found->set;
        }
        return m_graph.valueOf(node);
    }
    // The escaped objects that the library passes to callbacks, according to its declared function types.
    ValueSetID declaredCallbackArguments() const { return m_declaredCallbackArguments; }
    // The functions of the engine that came to the node before the unknown object covered them. Which they are hangs on the order of the rules: only the report reads this, for a place to name.
    ValueSetID functionsOfEngineListedAt(ValueFlowNodeID node) const
    {
        if (size_t share = node.index() / nodesPerShareOfListed; share < m_functionsOfEngineListed.size()) {
            const Vector<ValueFlowNodeSet>& listed = m_functionsOfEngineListed[share];
            const ValueFlowNodeSet* found = std::ranges::lower_bound(listed, node, { }, &ValueFlowNodeSet::node);
            if (found != listed.end() && found->node == node)
                return found->set;
        }
        return ValueSets::nothing;
    }
    // All that the set stands for: its members, and what they cover now that the solve has ended.
    ValueSetID withWhatMembersCover(ValueSetID, ValueSetMaker&) const;
    enum class GivenToNode : uint8_t { Nothing, UnknownObject, ValueNobodyKnows, WhatAnotherNodeOfBodyHolds };
    GivenToNode whatIsGivenTo(ValueFlowNodeID) const;
    ValueFlowNodeID nodeWhoseValueIsGivenTo(ValueFlowNodeID) const;
    uint32_t numberOfNodesGivenMore() const { return m_numberOfNodesGivenMore; }
    // The part of it that hangs on no order: the objects of the program written under the name of the read that the node has its value from.
    ValueSetID writtenUnderNameOfReadSeenAt(ValueFlowNodeID node) const
    {
        std::span<const ValueFlowNodeSet> all = m_writtenUnderNamesOfReads.span();
        auto found = std::ranges::lower_bound(all, node.index(), { }, [](const ValueFlowNodeSet& entry) { return entry.node.index(); });
        return found == all.end() || found->node != node ? ValueSets::nothing : found->set;
    }
    bool isReached(FunctionID function) const { return m_isReached.get(function.index()); }
    bool lacksLibraryFunction(RuntimeFunctionID function) const { return m_libraryFunctionsLacked.get(function.index()); }
    const BitVector& libraryFunctionsLacked() const LIFETIME_BOUND { return m_libraryFunctionsLacked; }
    OptionSet<BuiltinUse> usesOf(BuiltinID builtin) const { return builtin.index() < m_usesOfBuiltin.size() ? m_usesOfBuiltin[builtin.index()] : OptionSet<BuiltinUse>(); }
    OptionSet<ObjectMutation> marksOf(ValueFlowObjectID object) const { return m_marksOfObject[object.index()]; }

    bool holdsObjectOutsideFamily(ValueFlowNodeID node) const { return m_holdsObjectOutsideFamily.get(node.index()); }

    uint64_t numberOfRunsThatWereNotDueAndAdded() const { return m_numberOfRunsThatWereNotDueAndAdded; }
    std::span<const ValueFlowStatementID> firstStatementsThatWereNotDueAndAdded() const LIFETIME_BOUND { return m_firstStatementsThatWereNotDueAndAdded.span(); }
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

    ValueFlowReachWitness reachedBy(FunctionID function) const { return m_reachedBy[function.index()]; }
    Vector<ValueFlowNodeID> explain(ValueFlowNodeID, ValueFlowObjectID member) const;
    ValueSetID closuresFollowedToConstruction(ValueFlowNodeID callee) const
    {
        auto found = m_closuresFollowedToConstruction.find(callee.index());
        return found == m_closuresFollowedToConstruction.end() ? ValueSets::nothing : found->value;
    }
    ValueFlowStatementID statementThatLogged(ValueFlowEdge) const;
    struct LoggedEdge {
        ValueFlowEdge edge;
        ValueFlowStatementID statement;
        uint32_t round { 0 };
    };
    std::span<const LoggedEdge> edgesInOrderLogged() const LIFETIME_BOUND { return m_edgesInOrderLogged.span(); }
    const LoggedEdge* findLoggedEdge(ValueFlowEdge) const LIFETIME_BOUND;
    // For a statement created by a rule: the statement whose run first requested it.
    ValueFlowStatementID firstRequesterOf(ValueFlowStatementID) const;
    std::optional<ValueFlowUnknownSource> unknownSourceOf(ValueFlowEdge) const;
    std::optional<ValueFlowUnknownSource> unknownSourceOf(ValueFlowNodeID) const;
    std::optional<ValueFlowNodeID> findMadeNode(const ValueFlowNodeKey&) const;
    Subject subjectOf(ValueFlowNodeID) const;
    const ValueFlowNodeKey* keyOfMadeNode(ValueFlowNodeID node) const LIFETIME_BOUND { return node.index() < m_input.numberOfNodes ? nullptr : &m_madeNodes[m_madeNodesOfNode[node.index() - m_input.numberOfNodes]].key; }

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

    bool isMadeByRule(ValueFlowStatementID statement) const { return statement.index() >= m_input.statements.size(); }
    const ValueFlowNodeKey& keyOf(ValueFlowStatementID statement) const LIFETIME_BOUND { return m_madeStatements[statement.index() - m_input.statements.size()].key; }
    bool mayBeComputedKey(ValueFlowName) const;
    bool isFoundUnderComputedKey(ValueFlowName) const;
    bool isAtRest(ValueFlowStatementID) const;
    void forEachMadeNodeAskedForBy(ValueFlowStatementID statement, NOESCAPE const auto& functor) const
    {
        if (statement.index() >= m_firstUseOfStatement.size())
            return;
        for (uint32_t use = m_firstUseOfStatement[statement.index()]; use != UINT32_MAX; use = m_uses[use].next) {
            const MadeNodes& madeNodes = m_madeNodes[m_uses[use].madeNodes];
            if constexpr (Invocable<decltype(functor), void(const ValueFlowNodeKey&, ValueFlowStatementID, ValueFlowNodeID)>)
                functor(madeNodes.key, madeNodes.statement, madeNodes.first);
            else
                functor(madeNodes.key, madeNodes.statement);
        }
    }

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

    uint32_t indexOfMadeNodes(const ValueFlowNodeKey& key) const { return static_cast<uint32_t>(reinterpret_cast<const MadeNodes*>(&key) - m_madeNodes.span().data()); }
    const ValueFlowNodeKey& keyOfMadeNodesAt(uint32_t index) const LIFETIME_BOUND { return m_madeNodes[index].key; }
    ValueFlowNodeID firstOfMadeNodesAt(uint32_t index) const { return m_madeNodes[index].first; }
    uint32_t numberOfUsesOfMadeNodesAt(uint32_t index) const { return index < m_numberOfUsers.size() ? m_numberOfUsers[index] : 0; }
    ValueFlowStatementID statementOfMadeNodesAt(uint32_t index) const { return m_madeNodes[index].statement; }
    uint32_t numberOfRecordsOfMadeNodes() const { return m_madeNodes.size(); }

    const Counters& counters() const { return m_counters; }
    std::span<const Round> rounds() const LIFETIME_BOUND { return m_roundsDone.span(); }

private:
    friend class ValueFlowRuleContext;

    struct MadeNodes {
        ValueFlowNodeKey key;
        ValueFlowNodeID first;
        ValueFlowStatementID statement;
        bool staysAtRest { false };
        bool hasBeenAtRest { false };
        bool isListedUnderName { false };
        bool isListedUnderMembers { false };
        bool isUnionOfParts { false };
        bool isOfThisTurn { false };
        bool hasStatementRunWhereMade { false };
        // Of a union of two blocks: one more than the higher rank of the two. A record that is no such union is a block of rank 0.
        static constexpr uint8_t partsAreBlocksOfFallingRank = UINT8_MAX;
        uint8_t rank { 0 };
        uint32_t firstPart { 0 };
        uint32_t numberOfParts { 0 };
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

    // There are hundreds of thousands of these lists, and most hold one or two elements.
    template<typename T>
    using ShortList = Vector<T, 0, CrashOnOverflow, 2>;

    struct NodesOfName {
        ShortList<NodeOfSet> ofFewObjects;
        ShortList<NodeOfSet> ofManyObjects;
        ShortList<NodeOfSet> ofVeryManyObjects;
    };

    struct Name {
        ValueFlowName name;
        NodesOfName read;
        NodesOfName written;
        ValueSetID holders;
        uint32_t roundOfFirstQuestion { 0 };
        uint32_t firstPartWatched { UINT32_MAX };
    };

    struct KindOfNames {
        NodesOfName read;
        NodesOfName written;
        uint64_t readKindsThatMayFindIt { 0 };
        uint64_t writtenKindsItMayFind { 0 };
    };
    static constexpr unsigned mostKindsOfNames = 64;
    static constexpr uint32_t numberOfFirstKindAmongNames = 1u << 30;

    struct Covering {
        ValueFlowObjectID placeholder;
        ValueFlowNodeID covered;
        ValueSetID objectsThatMayBeCovered { ValueSets::nothing };
        ValueSetID allButCovered { ValueSets::anything };
    };

    struct KindThatCoversKeys {
        ValueFlowObjectID kind;
        ValueSetID allButKeys;
    };

    struct ObjectOutsideFamily {
        ValueFlowNodeID node;
        ValueFlowObjectID object;
    };
    String describeWayOf(const ObjectOutsideFamily&) const;

    struct Thread;

    std::optional<ValueFlowNodeID> findNodeMadeBeforeThisTurn(const ValueFlowNodeKey&) const;
    std::optional<uint32_t> findOrMakeNodesWhereAskedFor(const ValueFlowNodeKey&, uint32_t hash, ValueFlowRoundLog&, ValueSetMaker&) const;
    std::span<const ValueFlowOperand> guardsOf(const ValueFlowStatement& statement) const LIFETIME_BOUND { return m_input.operands.subspan(statement.firstGuard, statement.guardCount); }
    std::span<const ValueFlowOperand> operandsOf(const ValueFlowStatement& statement) const LIFETIME_BOUND { return m_input.operands.subspan(statement.firstOperand, statement.operandCount); }
    bool isKnownWatcher(ValueFlowNodeID node, ValueSetID within, ValueFlowStatementID statement) const
    {
        if (ValueFlowRoundLog::isNumberedByLog(node))
            return false;
        const WatchShard& shard = m_watchShards[watchShardOf(node, within)];
        const uint32_t* watch = shard.watchOfPart.find(static_cast<uint64_t>(node.index()) << 32 | within.index());
        return watch && shard.knownWatchers.contains(*watch, statement.index());
    }
    uint32_t numberOfName(ValueFlowName name) const
    {
        auto number = m_numberOfName.find(static_cast<uint64_t>(name.kind) << 32 | name.payload);
        return number != m_numberOfName.end() ? number->value : UINT32_MAX;
    }
    ValueSetID holdersUnder(uint32_t numberOfName) const { return numberOfName != UINT32_MAX ? m_names[numberOfName].holders : ValueSetID(); }
    ValueSetID partOfNameWatchedBy(ValueFlowStatementID statement, uint32_t numberOfName) const
    {
        if (numberOfName == UINT32_MAX)
            return { };
        const LinkShard& shard = m_linkShards[linkShardOf(numberOfName)];
        auto part = shard.partWatchedOf.find(static_cast<uint64_t>(numberOfName) << 32 | statement.index());
        return part != shard.partWatchedOf.end() ? shard.partsWatched[part->value].within : ValueSetID();
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
    ValueSetID valueForReadersOf(ValueFlowNodeID, ValueSetMaker&) const;
    ValueSetID valueShownTo(ValueFlowStatementID, ValueFlowNodeID) const;
    ValueSetID familyObjectsOf(ValueSetID mask, ValueSetMaker&) const;

    uint64_t contentOf(ValueFlowStatementID) const;
    void makeDue(ValueFlowStatementID);
    void listIfItMayBecomeDue(ValueFlowStatementID, Vector<ValueFlowStatementID>&) const;
    void makeStatementsDue(uint32_t first, uint32_t end);
    void makeEveryStatementDue();
    bool reach(uint32_t body, ValueFlowReachWitness);
    void reachFunctionsFoundBy(Thread&);
    void makeDueInRunningLoop(Thread&, ValueFlowStatementID);
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
        uint32_t madeNodes { UINT32_MAX };
    };
    class NodeMaker;
    std::optional<uint32_t> findNumberOfMadeNodes(const ValueFlowNodeKey&, uint32_t hash) const;
    std::optional<uint32_t> findNumberOfMadeNodes(const ValueFlowNodeKey& key) const { return findNumberOfMadeNodes(key, key.hash()); }
    void makeNodesOf(std::span<KeyToMake>);
    void takeWhatWasMade(std::span<const std::unique_ptr<NodeMaker>>, std::span<KeyToMake>);
    std::optional<uint32_t> makeNodesWhereAskedFor(const ValueFlowNodeKey&, uint32_t hash, ValueFlowRoundLog&, ValueSetMaker&) const;
    void takeWhatRulesHaveMade();
    static ValueFlowStatement statementMadeWith(const MadeNodes&);
    void runStatementsMadeBy(Thread&);
    void runStatementsWokenBy(Thread&);
    bool isRecordAtRest(uint32_t madeNodes) const { return madeNodes < m_numberOfUsers.size() && !m_numberOfUsers[madeNodes]; }
    uint64_t contentOf(const ValueFlowNodeKey&) const;
    void fillNumbersThatNobodyGave();
    ValueFlowNodeID makeNodes(const ValueFlowNodeKey&);
    ValueFlowNodeID ensureNodes(const ValueFlowNodeKey&);
    static void listToLink(const MadeNodes&, auto& readToLink, auto& writtenToLink);
    void linkNamedNodes(unsigned shard, bool isRead, ValueSetMaker&);
    struct Linked;
    struct NodeToLink;
    void link(const NodeOfSet&, bool isRead, NodesOfName& others, uint32_t numberOfName, Linked&);
    size_t connectWhereSetsMeet(const NodeOfSet&, bool isRead, std::span<const NodeOfSet> others, Linked&);
    void linkNode(const NodeToLink&, bool isRead, Linked&);
    void insertNode(const NodeToLink&, bool isRead);
    void insertNodesOfShards(const ListWithRoomToFill<NodeToLink>&, bool isRead);
    void numberNamesWatched();
    void noteNamesWatched(unsigned shard, ValueSetMaker&);
    void wakeWatchersOfNamesWrittenOnMoreObjects(unsigned shard, ValueSetMaker&);
    void removeNodesAtRestFrom(ShortList<NodeOfSet>&);
    void linkInChunks(bool isRead, unsigned numberOfOtherTasks, NOESCAPE const ScopedLambda<void(unsigned task, unsigned worker)>& otherTask);
    void noteReaders(unsigned shard, ValueSetMaker&);
    size_t numberOfReaders() const;
    void noteNodesAskedFor();
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
    void watch(WatchShard&, ValueFlowNodeID, ValueSetID within, ValueFlowStatementID, bool isAskedLast);
    void chainByMember(WatchShard&, uint32_t watchedNode, uint32_t watch);
    void chainSetsWatchedForCover();
    struct NewMemberOfWatchedNode {
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
    Vector<Vector<ValueFlowStatementID>> m_readersOfWorker;
    Vector<Vector<NewMemberOfWatchedNode>> m_newMembersOfWorker;
    Vector<Vector<WatchesToTest>> m_watchesToTestOfWorker;
    Vector<NewMemberOfWatchedNode> m_newMembers;
    void listReadersOfWatch(const WatchShard&, uint32_t watch, Vector<ValueFlowStatementID>& readers) const;
    ValueSetID withoutWhatOtherMembersCover(ValueFlowNodeID, ValueSetMaker&) const;
    ValueSetID withoutWhatOtherMembersCover(ValueSetID, ValueSetMaker&) const;
    ValueSetID withoutMarks(ValueSetID, ValueSetMaker&) const;
    ValueSetID findFamilyObjectsOf(ValueSetID mask, ValueSetMaker&) const;
    bool maySaturate(ValueFlowNodeID) const;
    bool isMadeForUnknownObject(ValueFlowNodeID) const;
    bool takesArgumentsOfCallGroup(ValueFlowNodeID) const;
    bool hasTooManyLayoutsAndCallees(ValueSetID);
    void saturate(std::span<const ValueFlowNodeID> moved);
    void leaveOutWhatIsCoveredInTheEnd();
    void spreadMarks();
    void findObjectsOutsideFamilies();
    ValueSetID findClosuresFollowedTo(ValueFlowNodeID, ValueSetMaker&) const;
    void findClosuresFollowedToConstructions();
    void findUnknownSources(NOESCAPE const ScopedLambda<void()>& whatRunsBesideTheWalk);
    void report();
    void dumpBytesByTable() const;
    static void forEachListOfOneRound(auto& solver, NOESCAPE const auto& functor);
    // The lists that each round fills anew keep the room that the largest round took. The rounds are largest in the middle of a solve, and the tables at its end.
    void giveBackRoomOfLargerRounds(uint64_t sizeOfRound);
    std::array<uint64_t, 8> m_sizesOfLatestRounds { };
    uint64_t m_sizeOfRoundThatListsHaveRoomFor { 0 };

    const ValueFlowSolverInput& m_input;
    ValueSets& m_sets;
    const ValueFlowRule m_rule;
    const ParallelLoops& m_loops;
    const Rounds m_rounds;

    Vector<std::unique_ptr<ValueSetMaker>> m_makers;
    ValueFlowGraph m_graph;
    Vector<std::unique_ptr<Thread>> m_threads;

    mutable ListThatStaysInPlace<MadeNodes> m_madeNodes;
    alignas(64) mutable std::atomic<uint32_t> m_nextNode { 0 };
    mutable std::atomic<uint32_t> m_nextStatement { 0 };
    alignas(64) uint32_t m_numberOfNodesMade { 0 };
    ListWithRoomToFill<uint32_t> m_numberOfUsers;
    ListWithRoomToFill<uint32_t> m_askerThatKeepsNothingAwake;
    struct alignas(64) IndexOfMadeNodes {
        using Slot = std::atomic<uint64_t>;

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

        void add(std::atomic<Table>& current, uint32_t hash, uint32_t number, size_t fewestSlots);
        void freeOutgrownTables();

        Lock lock;
        size_t size { 0 };
        Vector<Table, 2> tables;
    };
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
    alignas(64) mutable std::array<std::atomic<IndexOfMadeNodes::Table>, numberOfIndicesOfMadeNodes> m_tablesOfMadeNodes { };
    mutable std::array<IndexOfMadeNodes, numberOfIndicesOfMadeNodes> m_madeNodesByKey;
    size_t m_fewestSlotsOfIndexOfMadeNodes { 64 };
    ListWithRoomToFill<MadeStatement> m_madeStatements;
    ListWithRoomToFill<uint32_t> m_madeNodesOfNode;
    Vector<uint32_t> m_partsOfMadeNodes;
    using LatestNodes = std::array<uint32_t, 4>;
    struct NodesOfManyObjects {
        Vector<uint32_t> madeNodes;
        HashMap<uint64_t, LatestNodes, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> latest;
        HashMap<uint64_t, uint32_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> latestWithBlock;
        HashMap<uint32_t, std::array<uint64_t, 4>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> sampleOfSet;
        HashMap<uint64_t, Vector<ValueSetID>, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> cells;
    };
    static constexpr unsigned numberOfNodeMakers = 64;
    std::array<NodesOfManyObjects, numberOfNodeMakers> m_nodesOfManyObjects;
    Vector<uint32_t> m_madeNodesOfStatement;
    ListWithRoomToFill<uint32_t> m_firstUseOfStatement;
    ListThatStaysInPlace<Use> m_uses;
    Vector<uint32_t> m_freeUses;
    uint32_t m_numberOfNodesBeforeRound { 0 };
    uint32_t m_turnsOfRound { 0 };

    struct StepTimes {
        struct Step {
            ASCIILiteral name;
            Seconds wall;
            Seconds cpuOfThisThread;
            Seconds cpuOutsideParallelLoops;
        };

        void begin();
        void didFinish(ASCIILiteral step);
        void dump() const;

        bool isOn { false };
        MonotonicTime startOfStep;
        Seconds cpuAtStartOfStep;
        Seconds cpuInParallelLoopsAtStartOfStep;
        Vector<Step> steps;
        Seconds busyOnAllThreads;
        Seconds busiestThreadOfTurn;
        Seconds longestShareOfTurn;
    };
    StepTimes m_stepTimes;

    ValueFlowSharedBits m_isReached;
    ValueFlowSharedBits m_hasStatementOfBodyRun;
    uint64_t m_numberOfTurn { 0 };
    ValueFlowNodeID nodeAwaitedBy(ValueFlowStatementID, const ValueFlowStatement&) const;
    static constexpr uint32_t doesNotWait = UINT32_MAX;
    static constexpr uint32_t hasWaitedAndNotRun = UINT32_MAX - 1;
    bool m_statementsWait { false };
    ListWithRoomToFill<uint32_t> m_firstStatementWaitingForNode;
    ListWithRoomToFill<uint32_t> m_nextStatementWaiting;
    Vector<ValueFlowReachWitness> m_reachedBy;
    Vector<RunsAndEdges> m_runsAndEdges;
    uint64_t m_numberOfRunsThatWereNotDueAndAdded { 0 };
    bool m_usesGeneralRulesOnly { false };
    AddedByGeneralRules m_addedByGeneralRules;
    Vector<ValueFlowStatementID> m_firstStatementsThatWereNotDueAndAdded;
    HashMap<uint64_t, uint32_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_positionOfLoggedEdge;
    Vector<LoggedEdge> m_edgesInOrderLogged;
    HashMap<uint32_t, ValueFlowStatementID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_firstRequesterOfMadeNodes;
    Vector<uint32_t> m_bodiesReachedInRound;
    size_t m_paddingThatKeepsSharedListsOnTheirOwnCacheLines { 0 };

    mutable ValueFlowSharedBits m_isDue;
    BitVector m_isDear;
    Vector<ValueFlowStatementID> m_dearStatements;
    Vector<ValueFlowStatementID> m_due;
    uint32_t m_seedOfSchedule { 0 };
    uint32_t m_numberOfNodesGivenMore { 0 };
    Vector<ValueFlowStatementID> m_dueStatementsThatWait;
    void findStatementsWithLongRowsBehindThem();
    void letLongRowsGoFirst();
    bool hasLongRowBehindIt(ValueFlowStatementID statement) const { return m_hasLongRows && statement.index() < m_input.statements.size() && m_hasLongRowBehindIt.quickGet(statement.index()); }
    BitVector m_hasLongRowBehindIt;
    bool m_hasLongRows { false };
    mutable std::atomic<uint32_t> m_numberDueWithLongRows { 0 };
    Vector<ValueFlowStatementID> m_dueBehindLongRows;
    ValueFlowSharedBits m_hasRunInTurn;
    Vector<ValueFlowStatementID> m_dueInNextTurn;
    mutable ValueFlowSharedBits m_isDueLast;
    mutable Lock m_lockOfDueLast;
    mutable Vector<ValueFlowStatementID> m_dueLast;

    ListWithRoomToFill<uint32_t> m_firstReaderOfNode;
    ListWithRoomToFill<ValueSetID> m_seenByReadersOfNode;
    enum ListenerOfNode : uint8_t {
        WaitsForNodeToHoldSomething = 1 << 0,
        ReadsNode = 1 << 1,
        WatchesNode = 1 << 2,
        WaitsForFirstOperand = 1 << 3,
    };
    // The upper half of the byte counts how often a chain of prototypes has woken its readers.
    static constexpr unsigned shiftOfWakeUpsByChain = 4;
    AOT_CLASS_THRESHOLD(uint8_t, mostWakeUpsByGrowingChain, 15);
    bool isChainOfPrototypes(ValueFlowNodeID) const;
    void listAsDueLast(ValueFlowStatementID) const;
    void noteListener(ValueFlowNodeID node, ListenerOfNode listener) { WTF::atomicExchangeOr(&m_listenersOfNode[node.index()], static_cast<uint8_t>(listener), std::memory_order_relaxed); }
    ListWithRoomToFill<uint8_t> m_listenersOfNode;
    bool m_isFullRound { false };
    struct NodeRead {
        ValueFlowNodeID node;
        ValueFlowStatementID statement;
    };
    struct ReaderShard {
        Vector<Reader> readers;
        ValueFlowPairSet known;
        ValueFlowPairSet knownToWaitForSomething;
        HashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> firstReaderWaitingForSomething;
    };
    static constexpr unsigned numberOfReaderShards = 64;
    static constexpr unsigned readerShardOf(ValueFlowNodeID node) { return node.index() / 16 % numberOfReaderShards; }
    std::array<ReaderShard, numberOfReaderShards> m_readerShards;
    struct Watch {
        ValueSetID within;
        uint32_t firstReader { UINT32_MAX };
        uint32_t firstReaderAskingLast { UINT32_MAX };
        uint32_t watchedNode { 0 };
    };
    struct WatchedNode {
        ValueFlowNodeID node;
        ValueSetID seen;
        ShortList<uint32_t> watches;
        ShortList<uint32_t> watchesOfManyObjects;
        bool holdsWhatPlaceholderCovers { false };
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
    struct WatchShard {
        Vector<Reader> readers;
        Vector<Watch> watches;
        ValueFlowNumbersByKey watchOfPart;
        ValueFlowPairSet knownWatchers;
        Vector<WatchedNode> watchedNodes;
        HashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> watchedNodeOfNode;
        ValueFlowNumbersByKey firstWatchOfMember;
        Vector<ChainedWatch> watchesOfMembers;
        HashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> numberOfSetWatchedForCover;
        Vector<Vector<uint32_t, 2>> watchesOfSetWatchedForCover;
        std::array<Vector<MemberOfSetWatchedForCover>, numberOfRangesOfMembers> membersToChain;
        uint32_t endOfMembersToChain { 0 };
    };
    static constexpr unsigned numberOfWatchShardsByNode = 8;
    static constexpr unsigned numberOfWatchShardsBySet = 16;
    static constexpr unsigned numberOfWatchShards = numberOfWatchShardsByNode + numberOfWatchShardsBySet;
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
    Vector<uint32_t> m_firstSetWatchedForCoverOfMember;
    std::array<Vector<ChainedWatch>, numberOfRangesOfMembers> m_setsWatchedForCoverOfMembers;
    ValueSetID m_seenByWatchersOfEscaped;
    ValueSetID m_seenByWatchersOfLost;
    BitVector m_isWatched;
    HashMap<uint32_t, Vector<ValueFlowStatementID>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_readersOfFunction;
    HashMap<uint32_t, Vector<ValueFlowNodeSet, 1>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_valuesShownToStatement;
    mutable ListWithRoomToFill<ValueSetID> m_rememberedByStatement;

    Vector<Covering, 2> m_coverings;
    ValueSetID m_takenByRetiredNodes { ValueSets::nothing };
    // Sorted by node.
    ParallelArray<ValueFlowNodeSet> m_writtenUnderNamesOfReads;
    ValueSetID m_keptOutOfNodesMadeForUnknownObject { ValueSets::nothing };
    static constexpr uint32_t nodesPerShareOfListed = 1 << 12;
    Vector<Vector<ValueFlowNodeSet>> m_listedWithCoveredMembers;
    ValueSetID m_declaredCallbackArguments { ValueSets::nothing };
    Vector<Vector<ValueFlowNodeSet>> m_functionsOfEngineListed;
    bool m_haveLostObjectsEscaped { false };
    ValueSetID m_objectsThatPlaceholdersMayCover { ValueSets::nothing };
    ValueSetID m_objectsAlwaysFollowed { ValueSets::nothing };
    ValueSetID m_objectsLookedForBesideUnknownObject { ValueSets::nothing };
    ValueSetID m_allButMarks { ValueSets::anything };
    ValueSetID m_objectsSaidToBeSomething { ValueSets::nothing };
    ValueSetID m_objectsOfProgram { ValueSets::nothing };
    Vector<KindThatCoversKeys, 2> m_kindsThatCoverKeys;
    HashMap<uint32_t, ValueSetID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_familyObjectsOfMask;

    bool m_saturates { false };
    ValueSetID m_untrackedObjectAlone;
    bool m_hasSaidThatAllElseIsAtRest { false };
    ListWithRoomToFill<uint32_t> m_layoutOrCalleeOfMember;
    ListWithRoomToFill<uint32_t> m_lastCountThatSaw;
    uint32_t m_numberOfCounts { 0 };
    HashMap<uint32_t, bool, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_isTooLarge;
    BitVector m_isSaturated;

    Vector<Name> m_names;
    std::array<KindOfNames, mostKindsOfNames> m_kindsOfNames;
    HashMap<uint64_t, uint32_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_numberOfName;
    struct ChainedNode {
        ValueFlowNodeID node;
        uint32_t next { UINT32_MAX };
    };
    static constexpr unsigned numberOfLinkShardsOfNames = 64;
    static constexpr unsigned numberOfLinkShardsOfKinds = 8;
    static constexpr unsigned numberOfLinkShards = numberOfLinkShardsOfNames + numberOfLinkShardsOfKinds;
    static constexpr unsigned linkShardOf(uint32_t numberOfName)
    {
        if (numberOfName >= numberOfFirstKindAmongNames)
            return numberOfLinkShardsOfNames + (numberOfName - numberOfFirstKindAmongNames) % numberOfLinkShardsOfKinds;
        return numberOfName % numberOfLinkShardsOfNames;
    }
    enum class SizeOfSetToLink : uint8_t {
        Few,
        Many,
        VeryMany,
    };
    struct NodesOfMembers {
        ValueFlowNumbersByKey first;
        Vector<ChainedNode> chained;
    };
    struct Linked {
        struct LongList {
            NodeOfSet node;
            bool isRead { false };
            ShortList<NodeOfSet>* others { nullptr };
        };

        Vector<ValueFlowEdge> edges;
        uint64_t testsWhetherSetsMeet { 0 };
        Vector<ShortList<NodeOfSet>*> listsMostlyAtRest;
        Vector<LongList> longLists;
    };
    void connectAcrossLongLists(std::span<const Linked::LongList>);
    struct PartOfNameWatched {
        ValueFlowStatementID statement;
        ValueSetID within;
        uint32_t round { 0 };
        uint32_t next { UINT32_MAX };
    };
    struct LinkShard {
        std::array<NodesOfMembers, 2> nodesOfMembers;
        Linked linked;
        Vector<PartOfNameWatched> partsWatched;
        HashMap<uint64_t, uint32_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> partWatchedOf;
        struct NameWatched {
            uint32_t numberOfName { 0 };
            ValueSetID within;
            ValueFlowStatementID statement;
        };
        Vector<NameWatched> namesWatchedInRound;
        Vector<std::pair<uint32_t, ValueSetID>> writtenUnderWatchedNames;
        Vector<ValueFlowStatementID> woken;
    };
    std::array<LinkShard, numberOfLinkShards> m_linkShards;
    struct NodeToLink {
        NodeOfSet node;
        ValueFlowName name;
        bool standsForKind { false };
        SizeOfSetToLink size { SizeOfSetToLink::Few };
        bool isDearToLink { false };
        uint32_t numberOfName { 0 };
        uint32_t madeNodes { 0 };
        ValueFlowObjectID onlyMember;
    };
    ListWithRoomToFill<NodeToLink> m_readToLink;
    ListWithRoomToFill<NodeToLink> m_writtenToLink;
    ListWithRoomToFill<NodeToLink> m_readToInsert;
    std::array<Vector<uint32_t>, numberOfLinkShards> m_nodesToInsertOfShards;
    RefPtr<BackgroundTask> m_insertionOfNodesThatRead;

    Vector<ValueFlowEdge> m_edgesOfRound;

    Vector<ValueFlowEdge> m_edgesInFrontOfLinks;
    Vector<ValueFlowGraph::StoppedPlaceholder> m_placeholdersStopped;
    Vector<OptionSet<ObjectMutation>> m_marksOfObject;
    Vector<OptionSet<BuiltinUse>> m_usesOfBuiltin;
    BitVector m_statementsWithMembersNotToldApart;
    BitVector m_statementsThatAskProxiesByTheirTraps;
    BitVector m_libraryFunctionsLacked;
    ValueSetID followedProxiesThatMoreMayBeDoneTo();
    bool wakeReadersOfNodesThatShowSomethingElse();
    Vector<ValueSetID> m_uncoveredAtLastReaderWake;
    uint16_t m_stateOfCoverings { 0 };
    mutable ListWithRoomToFill<uint16_t> m_coveringStateOfShownView;
    Vector<ObjectOutsideFamily> m_objectsOutsideFamilies;
    HashMap<uint32_t, ValueSetID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_closuresFollowedToConstruction;
    BitVector m_holdsObjectOutsideFamily;
    Vector<uint8_t> m_unknownSourceOfNode;

    Counters m_counters;
    Vector<Round> m_roundsDone;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
