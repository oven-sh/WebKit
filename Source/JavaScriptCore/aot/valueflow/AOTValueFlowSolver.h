/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTParkedBodies.h"
#include "AOTBackgroundTask.h"
#include "AOTCompactList.h"
#include "AOTSystemResources.h"
#include "AOTThreads.h"
#include "AOTThreshold.h"
#include "AOTValueFlowGraph.h"
#include "AOTValueFlowNumberLists.h"
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

class ValueFlowBodies;

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
    // Where no later solve reads them: the owner of the two lists above, which lets go of them once the graph has them.
    ValueFlowBodies* ownerOfEdgesAndMasks { nullptr };
    std::span<const ValueFlowStatement> statements;
    std::span<const ValueFlowOperand> operands;
    std::span<const ValueFlowRareFactsOfText> rareFactsOfText;
    std::span<const ValueFlowCopyOfOperation> copiesOfOperations;
    std::span<const ValueFlowEdge> edgesOnceReached;
    std::span<const ValueFlowNodeSet> seedsOnceReached;
    std::span<const FunctionID> functionsEnteredOnceReached;
    std::span<const ValueFlowBodyRange> ranges;

    uint64_t mostSetMembers { UINT64_MAX };
    uint64_t mostMembersToldApart { UINT64_MAX };
    uint64_t mostObjectsListedByMember { UINT64_MAX };
    bool remembersWhoLoggedEachEdge { false };
    bool isAskedHowValuesCame { true };
    bool leavesOutWhatIsCovered { false };
    // Counts, limits that were met, errors of validation, and the lines of the rounds in the log: whoever counts those counts builds.
    bool reportsToBuild { true };
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
        // Room for half as much again: a list that got exactly what a large round asked for is full, and the next few elements would copy all of it. A page that nobody writes to costs nothing.
        if (m_size + more > capacity())
            m_elements.realloc(std::max<size_t>({ 1024, capacity() * 2, m_size + more + (m_size + more) / 2 }) * sizeof(T));
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
    void release();

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
        size_t size = m_slots.span().size();
        for (size_t position = firstPositionOf(key + 1, size);; position = position + 1 == size ? 0 : position + 1) {
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
    static size_t firstPositionOf(uint64_t keyPlusOne, size_t size)
    {
        uint64_t hash = keyPlusOne * 0x9e3779b97f4a7c15ull;
        return static_cast<size_t>(static_cast<UInt128>((hash ^ hash >> 32) * 0xbf58476d1ce4e5b9ull) * size >> 64);
    }
    void grow();

    MallocSpan<Slot> m_slots;
    size_t m_size { 0 };
};

// From the hash of a key to the number of the record that holds the key. A slot holds one more than the number, below five bits of the hash.
class ValueFlowNumbersOfRecords {
public:
    static uint64_t hashOf(uint64_t key)
    {
        uint64_t hash = (key + 1) * 0x9e3779b97f4a7c15ull;
        return (hash ^ hash >> 32) * 0xbf58476d1ce4e5b9ull;
    }

    ALWAYS_INLINE std::optional<uint32_t> find(uint64_t hash, NOESCAPE const Invocable<bool(uint32_t number)> auto& holdsKey) const
    {
        if (!m_slots)
            return std::nullopt;
        size_t size = m_slots.span().size();
        for (size_t position = firstPositionOf(hash, size);; position = position + 1 == size ? 0 : position + 1) {
            uint32_t slot = m_slots[position];
            if (!slot)
                return std::nullopt;
            if (!((slot ^ bitsOfHashIn(hash)) & ~maskOfNumber) && holdsKey((slot & maskOfNumber) - 1))
                return (slot & maskOfNumber) - 1;
        }
    }
    // The key is not in the table.
    void add(uint64_t hash, uint32_t number, NOESCAPE const Invocable<uint64_t(uint32_t number)> auto& hashOfRecord)
    {
        if (isFullFor(m_size + 1))
            grow(hashOfRecord);
        put(hash, number);
        ++m_size;
    }
    size_t bytesInUse() const { return m_size * sizeof(uint32_t); }
    size_t memoryInBytes() const { return m_slots.sizeInBytes(); }

private:
    static constexpr unsigned bitsOfNumber = 27;
    static constexpr uint32_t maskOfNumber = (1u << bitsOfNumber) - 1;
    static uint32_t bitsOfHashIn(uint64_t hash) { return static_cast<uint32_t>(hash) << bitsOfNumber; }
    static size_t firstPositionOf(uint64_t hash, size_t size) { return static_cast<size_t>(static_cast<UInt128>(hash) * size >> 64); }
    bool isFullFor(size_t numberOfKeys) const { return numberOfKeys * 4 > m_slots.span().size() * 3; }
    void put(uint64_t hash, uint32_t number)
    {
        if (number + 1 > maskOfNumber) [[unlikely]]
            exitProgramTooLarge();
        size_t size = m_slots.span().size();
        size_t position = firstPositionOf(hash, size);
        while (m_slots[position])
            position = position + 1 == size ? 0 : position + 1;
        m_slots[position] = bitsOfHashIn(hash) | (number + 1);
    }
    void grow(NOESCAPE const Invocable<uint64_t(uint32_t number)> auto& hashOfRecord)
    {
        size_t size = std::max<size_t>(m_slots.span().size() + m_slots.span().size() / 2, 64);
        MallocSpan<uint32_t> previous = std::exchange(m_slots, MallocSpan<uint32_t>::malloc(size * sizeof(uint32_t)));
        zeroSpan(m_slots.mutableSpan());
        for (uint32_t slot : previous.span()) {
            if (slot)
                put(hashOfRecord((slot & maskOfNumber) - 1), (slot & maskOfNumber) - 1);
        }
    }

    MallocSpan<uint32_t> m_slots;
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
        uint64_t readers { 0 };
        uint64_t testsWhetherSetsMeet { 0 };
        uint32_t functionsReached { 0 };
        uint32_t madeNodesAtRest { 0 };
        uint32_t nodesSaturated { 0 };
        uint32_t nodesHoldingObjectsOutsideFamily { 0 };
        uint32_t namesWithSeveralCells { 0 };
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

    void solve();

    const ValueSets& sets() const { return m_sets; }
    const ValueFlowGraph& graph() const { return m_graph; }
    std::span<const std::unique_ptr<ValueSetMaker>> makersIdleAfterSolve() const LIFETIME_BOUND { return m_makers.span(); }
    // Everything that may reach the node. A proof reads this. A member that another member covers is left out.
    ValueSetID possibleValuesOf(ValueFlowNodeID node) const { return m_graph.valueOf(node); }
    // What the program itself brings to the node. A prediction reads this: it may name too few, so nothing is proven from it.
    ValueSetID membersThatOtherMembersCover(ValueSetID value, ValueSetMaker& maker) const { return maker.intersect(value, ValueSets::complement(withoutWhatOtherMembersCover(value, maker))); }
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
    // From the first round until the rows are released the bodies of functions may be parked. A record of a function is read as ever; whoever reads blocks or values
    // takes the function from here, or has asked here first: the body comes back in place.
    void readBodiesThrough(ParkedBodies& parked) { m_parkedBodies = &parked; }
    const Function& functionWithBody(FunctionID id) const { return m_parkedBodies ? m_parkedBodies->at(id) : m_input.program->at(id); }
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
    uint32_t numberOfStatements() const { return m_input.statements.size() + m_madeNodesOfStatement.size(); }
    // A statement made by a rule follows from its record. While the rounds run none is kept; behind them they are laid out, so that whoever reads the results is handed one that stays where it is.
    const ValueFlowStatement& statementAt(ValueFlowStatementID statement) const LIFETIME_BOUND { return isMadeByRule(statement) ? m_statementsMadeByRules[statement.index() - m_input.statements.size()] : m_input.statements[statement.index()]; }

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
    // A node keeps what an edge brought it after the edge is removed.
    void forEachPredecessorIncludingRemoved(ValueFlowNodeID node, NOESCAPE const Invocable<void(ValueFlowNodeID)> auto& functor) const
    {
        m_graph.forEachPredecessor(node, functor);
        if (auto removed = m_sourcesOfRemovedEdges.find(node.index()); removed != m_sourcesOfRemovedEdges.end()) {
            for (ValueFlowNodeID predecessor : removed->value)
                functor(predecessor);
        }
    }
    // For a statement created by a rule: the statement whose run first requested it.
    ValueFlowStatementID firstRequesterOf(ValueFlowStatementID) const;
    std::optional<ValueFlowUnknownSource> unknownSourceOf(ValueFlowEdge) const;
    std::optional<ValueFlowUnknownSource> unknownSourceOf(ValueFlowNodeID) const;
    // Behind the rounds this makes the index of all keys again: it is for tools.
    std::optional<ValueFlowNodeID> findMadeNode(const ValueFlowNodeKey&) const;
    std::optional<ValueFlowNodeID> markersOf(FunctionID function) const { return m_markersOfFunctions.getOptional(function.index()); }
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
    const ValueFlowNodeKey& keyOf(ValueFlowStatementID statement) const LIFETIME_BOUND
    {
        static constexpr ValueFlowNodeKey keyOfStatementWithoutRecord;
        uint32_t madeNodes = m_madeNodesOfStatement[statement.index() - m_input.statements.size()];
        return madeNodes != UINT32_MAX ? m_madeNodes[madeNodes].key : keyOfStatementWithoutRecord;
    }
    bool mayBeComputedKey(ValueFlowName) const;
    bool isFoundUnderComputedKey(ValueFlowName) const;
    bool isAtRest(ValueFlowStatementID) const;
    // For whoever has copied them.
    void letGoOfRecordsAskedFor();
    // Behind this numberOfUsesOfMadeNodesAt() is not to be asked.
    void letGoOfNumbersOfUsers();
    void forEachMadeNodeAskedForBy(ValueFlowStatementID statement, NOESCAPE const auto& functor) const
    {
        RELEASE_ASSERT(m_knowsRecordsAskedFor);
        forEachRecordAskedForBy(statement.index(), [&](uint32_t index) {
            const MadeNodes& madeNodes = m_madeNodes[index];
            if constexpr (Invocable<decltype(functor), void(const ValueFlowNodeKey&, ValueFlowStatementID, ValueFlowNodeID)>)
                functor(madeNodes.key, madeNodes.statement(), madeNodes.first);
            else
                functor(madeNodes.key, madeNodes.statement());
        });
    }

    void forEachMadeNodeAskedForByStatements(uint32_t first, uint32_t end, NOESCAPE const Invocable<void(uint32_t statement)> auto& beginStatement, NOESCAPE const Invocable<void(uint32_t indexOfMadeNodes, const ValueFlowNodeKey&, ValueFlowStatementID)> auto& functor) const
    {
        RELEASE_ASSERT(m_knowsRecordsAskedFor);
        constexpr uint32_t listsSideBySide = 16;
        std::array<Vector<uint32_t, 32>, listsSideBySide> found;
        for (uint32_t base = first; base < end; base += listsSideBySide) {
            uint32_t count = std::min(listsSideBySide, end - base);
            for (uint32_t list = 0; list < count; ++list) {
                if (base + list < m_usesOfStatement.size() && m_usesOfStatement[base + list] != UINT32_MAX)
                    __builtin_prefetch(&m_uses[m_usesOfStatement[base + list]]);
            }
            for (uint32_t list = 0; list < count; ++list) {
                found[list].shrink(0);
                forEachRecordAskedForBy(base + list, [&](uint32_t index) {
                    found[list].append(index);
                    __builtin_prefetch(&m_madeNodes[index]);
                });
            }
            for (uint32_t list = 0; list < count; ++list) {
                beginStatement(base + list);
                for (uint32_t index : found[list])
                    functor(index, m_madeNodes[index].key, m_madeNodes[index].statement());
            }
        }
    }

    uint32_t indexOfMadeNodes(const ValueFlowNodeKey& key) const { return static_cast<uint32_t>(reinterpret_cast<const MadeNodes*>(&key) - m_madeNodes.span().data()); }
    const ValueFlowNodeKey& keyOfMadeNodesAt(uint32_t index) const LIFETIME_BOUND { return m_madeNodes[index].key; }
    ValueFlowNodeID firstOfMadeNodesAt(uint32_t index) const { return m_madeNodes[index].first; }
    uint32_t numberOfUsesOfMadeNodesAt(uint32_t index) const
    {
        RELEASE_ASSERT(m_knowsNumbersOfUsers);
        return index < m_numberOfUsers.size() ? m_numberOfUsers[index] : 0;
    }
    ValueFlowStatementID statementOfMadeNodesAt(uint32_t index) const { return m_madeNodes[index].statement(); }
    uint32_t numberOfRecordsOfMadeNodes() const { return m_madeNodes.size(); }

    const Counters& counters() const { return m_counters; }
    std::span<const Round> rounds() const LIFETIME_BOUND { return m_roundsDone.span(); }

private:
    friend class ValueFlowRuleContext;

    struct MadeNodes {
        enum Flag : uint8_t {
            StaysAtRest = 1 << 0,
            HasBeenAtRest = 1 << 1,
            IsListedUnderName = 1 << 2,
            IsListedUnderMembers = 1 << 3,
            IsUnionOfParts = 1 << 4,
            IsOfThisTurn = 1 << 5,
            HasStatementRunWhereMade = 1 << 6,
            HasParts = 1 << 7,
        };
        // Threads that look after different things write to different flags of one record at a time.
        bool has(Flag flag) const { return WTF::atomicLoad(const_cast<uint8_t*>(&key.flagsOfRecord), std::memory_order_relaxed) & flag; }
        bool set(Flag flag) { return WTF::atomicExchangeOr(&key.flagsOfRecord, static_cast<uint8_t>(flag), std::memory_order_relaxed) & flag; }
        bool clear(Flag flag) { return WTF::atomicExchangeAnd(&key.flagsOfRecord, static_cast<uint8_t>(~flag), std::memory_order_relaxed) & flag; }
        void setTo(Flag flag, bool value) { value ? set(flag) : clear(flag); }

        // A record has a statement or parts, never both.
        ValueFlowStatementID statement() const { return has(HasParts) ? ValueFlowStatementID() : ValueFlowStatementID(statementOrFirstPart); }
        void setStatement(ValueFlowStatementID statement)
        {
            RELEASE_ASSERT(!has(HasParts));
            statementOrFirstPart = statement.index();
        }
        uint32_t firstPart() const
        {
            ASSERT(has(HasParts));
            return statementOrFirstPart;
        }
        void setFirstPart(uint32_t firstPart)
        {
            RELEASE_ASSERT(has(HasParts) || statementOrFirstPart == UINT32_MAX);
            set(HasParts);
            statementOrFirstPart = firstPart;
        }

        ValueFlowNodeKey key;
        ValueFlowNodeID first;
        uint32_t statementOrFirstPart { UINT32_MAX };
        // Of a union of two blocks: one more than the higher rank of the two. A record that is no such union is a block of rank 0.
        static constexpr uint8_t partsAreBlocksOfFallingRank = UINT8_MAX;
        uint8_t rank() const { return key.rankOfRecord; }
        void setRank(uint8_t rank) { key.rankOfRecord = rank; }
    };
    static_assert(sizeof(MadeNodes) == 20);
    // The parts of a record stand side by side. The last of them has this bit.
    static constexpr uint32_t isLastPart = 1u << 31;
    static void forEachPartFrom(const uint32_t* part, NOESCAPE const Invocable<void(uint32_t madeNodes)> auto& functor)
    {
        WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
        for (;; ++part) {
            functor(*part & ~isLastPart);
            if (*part & isLastPart)
                return;
        }
        WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
    }
    void forEachPartOf(const MadeNodes& madeNodes, NOESCAPE const Invocable<void(uint32_t madeNodes)> auto& functor) const
    {
        if (madeNodes.has(MadeNodes::HasParts))
            forEachPartFrom(&m_partsOfMadeNodes[madeNodes.firstPart()], functor);
    }

    bool keepsAwake(ValueFlowStatementID asker, uint32_t madeNodes) const { return !m_madeNodes[madeNodes].has(MadeNodes::StaysAtRest) && m_madeNodes[madeNodes].statement() != asker; }

    // The records that a statement asked for stand side by side, the latest first. The last of them has this bit.
    static constexpr uint32_t isLastUse = 1u << 31;
    bool anyUseIn(uint32_t list, NOESCAPE const Invocable<bool(uint32_t madeNodes)> auto& functor) const
    {
        if (list == UINT32_MAX)
            return false;
        WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN
        for (const uint32_t* use = &m_uses[list];; ++use) {
            if (functor(*use & ~isLastUse))
                return true;
            if (*use & isLastUse)
                return false;
        }
        WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
    }
    void forEachUseIn(uint32_t list, NOESCAPE const Invocable<void(uint32_t madeNodes)> auto& functor) const
    {
        anyUseIn(list, [&](uint32_t madeNodes) {
            functor(madeNodes);
            return false;
        });
    }
    uint32_t usesOf(uint32_t statement) const { return statement < m_usesOfStatement.size() ? m_usesOfStatement[statement] : UINT32_MAX; }
    void forEachRecordAskedForBy(uint32_t statement, NOESCAPE const Invocable<void(uint32_t madeNodes)> auto& functor) const { forEachUseIn(usesOf(statement), functor); }

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
    using ListOfNodesOfSets = CompactList<NodeOfSet>;

    struct NodesOfName {
        ListOfNodesOfSets ofFewObjects;
        ListOfNodesOfSets ofManyObjects;
        ListOfNodesOfSets ofVeryManyObjects;
        // From a member to the list of the nodes whose sets hold it. The maps and the lists lie with the shard of the name.
        uint32_t ofFewObjectsByMember { 0 };
        uint32_t ofManyObjectsByMember { 0 };
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
    // Whether an operation of the program names the key, be it a read or a store, or native code asks for it by name. Any other name stands in literals alone, and what lies under it is found under keys alone.
    bool isAskedForByName(PropertyKeyID key) const { return m_namesAskedFor.get(key.index()); }
    std::span<const ValueFlowOperand> objectsReadUnderKeyOfStore(const ValueFlowStatement& statement) const LIFETIME_BOUND
    {
        size_t number = statement.factsOfText.contains(ValueFlowFactOfText::StoresWhatOneObjectHoldsUnderSameKey) ? 1 : statement.factsOfText.contains(ValueFlowFactOfText::StoresWhatTwoObjectsHoldUnderSameKey) ? 2 : 0;
        return m_input.operands.subspan(statement.firstOperand + statement.operandCount, number);
    }
    std::span<const ValueFlowOperand> operandsOf(const ValueFlowStatement& statement) const LIFETIME_BOUND { return m_input.operands.subspan(statement.firstOperand, statement.operandCount); }
    bool isKnownWatcher(ValueFlowNodeID node, ValueSetID within, ValueFlowStatementID statement) const
    {
        if (ValueFlowRoundLog::isNumberedByLog(node))
            return false;
        const WatchShard& shard = m_watchShards[watchShardOf(node, within)];
        std::optional<uint32_t> watch = shard.findWatchOf(node, within);
        return watch && shard.isReaderOf(*watch, statement);
    }
    uint32_t numberOfName(ValueFlowName name) const
    {
        return findNumberOfName(name).value_or(UINT32_MAX);
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
    // Three steps, each for what the one before has brought: the name of the node's list, its first words, the place of the statement in it.
    uint32_t readersOf(ValueFlowNodeID node) const { return node.index() < m_readersOfNode.size() ? m_readersOfNode[node.index()] : 0; }
    void prefetchReadersOf(ValueFlowNodeID node) const
    {
        if (node.index() < m_readersOfNode.size())
            __builtin_prefetch(&m_readersOfNode[node.index()]);
    }
    void prefetchFirstReadersOf(ValueFlowNodeID node) const { m_readerShards[readerShardOf(node)].readersOfNodes.prefetch(readersOf(node)); }
    void prefetchWhetherKnownReader(ValueFlowNodeID node, ValueFlowStatementID statement) const { m_readerShards[readerShardOf(node)].readersOfNodes.prefetchPlaceOf(readersOf(node), statement.index()); }
    void prefetchWhetherKnownToWaitForSomethingIn(ValueFlowNodeID node, ValueFlowStatementID statement) const
    {
        if (!ValueFlowRoundLog::isNumberedByLog(node))
            m_readerShards[readerShardOf(node)].knownToWaitForSomething.prefetch(node.index(), statement.index());
    }
    bool isKnownReader(ValueFlowNodeID node, ValueFlowStatementID statement) const { return m_readerShards[readerShardOf(node)].readersOfNodes.contains(readersOf(node), statement.index()); }
    void forEachReaderOf(ValueFlowNodeID node, NOESCAPE const Invocable<void(ValueFlowStatementID)> auto& functor) const
    {
        m_readerShards[readerShardOf(node)].readersOfNodes.forEach(m_readersOfNode[node.index()], [&](uint32_t statement) {
            functor(ValueFlowStatementID(statement));
        });
    }
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
    void runRules();
    void letSomeDueStatementsWait();
    void letStatementsThatRunOftenWait();
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
    ValueFlowStatement statementMadeByRuleAt(ValueFlowStatementID) const;
    ValueFlowStatement copyOfStatementAt(ValueFlowStatementID statement) const { return isMadeByRule(statement) ? statementMadeByRuleAt(statement) : m_input.statements[statement.index()]; }
    Vector<ValueFlowStatement> m_statementsMadeByRules;
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
    void removeNodesAtRestFrom(ListOfNodesOfSets&);
    void linkInChunks(bool isRead, unsigned numberOfOtherTasks, NOESCAPE const ScopedLambda<void(unsigned task, unsigned worker)>& otherTask);
    void noteReaders(unsigned shard, ValueSetMaker&);
    size_t numberOfReaders() const;
    void noteNodesAskedFor();
    // A list of up to 32 uses takes as many words as it has uses. A longer one takes the next power of two or three quarters of it.
    // A list that was given back holds the position of the next free one of its size in its first word.
    struct UsesOfThread {
        static constexpr uint32_t mostUsesWithoutSpareRoom = 32;
        static constexpr unsigned numberOfSizes = mostUsesWithoutSpareRoom + 1 + 2 * (32 - 5);
        static uint64_t roomFor(uint32_t count)
        {
            if (count <= mostUsesWithoutSpareRoom)
                return count;
            uint64_t powerOfTwo = std::bit_ceil(static_cast<uint64_t>(count));
            return count <= powerOfTwo / 4 * 3 ? powerOfTwo / 4 * 3 : powerOfTwo;
        }
        static unsigned sizeOf(uint64_t room)
        {
            if (room <= mostUsesWithoutSpareRoom)
                return room;
            return mostUsesWithoutSpareRoom + 1 + 2 * (std::bit_width(room) - 6) - std::has_single_bit(room);
        }
        static uint64_t roomOf(unsigned size)
        {
            if (size <= mostUsesWithoutSpareRoom)
                return size;
            unsigned above = size - mostUsesWithoutSpareRoom - 1;
            return (above % 2 ? 64ull : 48ull) << above / 2;
        }
        // The largest room that a list may have and that fits into so many words.
        static uint64_t largestRoomIn(uint64_t words)
        {
            if (words <= mostUsesWithoutSpareRoom)
                return words;
            uint64_t powerOfTwo = std::bit_floor(words);
            return powerOfTwo / 2 * 3 <= words ? powerOfTwo / 2 * 3 : powerOfTwo;
        }

        UsesOfThread() { firstFree.fill(UINT32_MAX); }

        std::array<uint32_t, numberOfSizes> firstFree;
        // A bit for each size: a list of that size is free.
        std::array<uint64_t, 2> sizesWithFreeLists { };
        size_t nextFresh { 0 };
        size_t endOfFresh { 0 };
    };
    uint32_t takeUses(UsesOfThread&, uint32_t count);
    void giveBackRoomOfUses(UsesOfThread&, uint32_t list, uint64_t room);
    void giveBackUses(UsesOfThread& uses, uint32_t list, uint32_t count) { giveBackRoomOfUses(uses, list, UsesOfThread::roomFor(count)); }
    Vector<UsesOfThread> m_usesOfThreads;
    template<typename T> void growAndFill(ListWithRoomToFill<T>&, size_t size, T value);
    void takeUpPartsOfNodesWithFirstUser(Vector<uint32_t>&& madeNodes);
    enum class WhatIsLetGo : bool { Records, PartsOfReadsAtRest };
    void letGoOf(Vector<uint32_t>&& madeNodes, WhatIsLetGo);
    static bool isReadMadeOfParts(const MadeNodes&);
    void letGoOfPartsOfReadsAtRest(NOESCAPE const Invocable<bool(const MadeNodes&)> auto& isLetGo);
    Vector<uint32_t> m_readsAtRestWhosePartsAreAwake;
    void forgetReadersOf(std::span<const ValueFlowNodeID>);
    bool isLaidByRule(ValueFlowEdge) const;
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
    void dumpReadersByStatement() const;
    void dumpEdgesByEnds() const;
    static void forEachListOfOneRound(auto& solver, NOESCAPE const auto& functor);
    // The lists that each round fills anew keep the room that the largest round took. The rounds are largest in the middle of a solve, and the tables at its end.
    void giveBackRoomOfLargerRounds(uint64_t sizeOfRound);
    enum class ListsOfLinks : bool { AreInUse, AreLookedAtToo };
    void giveBackRoomOfEmptyLists(ListsOfLinks);
    // Between two rounds. Every member that holds a set is listed there, and so is whatever else the rules write sets to.
    void freeSetsThatNobodyHolds();
    size_t m_bytesOfSetsBehindLastFreeing { 0 };
    bool m_freesSetsEveryRound { false };
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
    struct alignas(64) IndexOfMadeNodes {
        // One more than the number of the record, below five bits of the hash that neither the choice of the index nor the place in the table has used.
        // The results name a record in as many bits (ValueFlowNodesAskedFor).
        using Slot = std::atomic<uint32_t>;
        static constexpr unsigned bitsOfNumber = 27;
        static constexpr uint32_t maskOfNumber = (1u << bitsOfNumber) - 1;
        static constexpr uint32_t bitsOfHashIn(uint32_t hash) { return hash >> 17 << bitsOfNumber; }

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

        void add(std::atomic<Table>& current, uint32_t hash, uint32_t number, size_t fewestSlots, const ListThatStaysInPlace<MadeNodes>&);
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
            uint32_t slot = slots[index].load(std::memory_order_acquire);
            if (!slot)
                return std::nullopt;
            if (!((slot ^ IndexOfMadeNodes::bitsOfHashIn(hash)) & ~IndexOfMadeNodes::maskOfNumber) && isEqual((slot & IndexOfMadeNodes::maskOfNumber) - 1))
                return (slot & IndexOfMadeNodes::maskOfNumber) - 1;
        }
        WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
    }
    alignas(64) mutable std::array<std::atomic<IndexOfMadeNodes::Table>, numberOfIndicesOfMadeNodes> m_tablesOfMadeNodes { };
    mutable std::array<IndexOfMadeNodes, numberOfIndicesOfMadeNodes> m_madeNodesByKey;
    size_t m_fewestSlotsOfIndexOfMadeNodes { 64 };
    void releaseIndexOfMadeNodes();
    void makeIndexOfMadeNodesAgain() const;
    mutable std::atomic<bool> m_hasIndexOfMadeNodes { true };
    mutable Lock m_lockOfIndexMadeAgain;
    HashMap<uint32_t, ValueFlowNodeID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_markersOfFunctions;
    ListWithRoomToFill<uint32_t> m_madeNodesOfNode;
    Vector<uint32_t> m_partsOfMadeNodes;
    using LatestNodes = std::array<uint32_t, 4>;
    struct NodesOfManyObjects {
        Vector<uint32_t> madeNodes;
        HashMap<uint64_t, LatestNodes, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> latest;
        ValueFlowNumbersByKey latestWithBlock;
        HashMap<uint64_t, Vector<ValueSetID>, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> cells;
    };
    static constexpr unsigned numberOfNodeMakers = 64;
    std::array<NodesOfManyObjects, numberOfNodeMakers> m_nodesOfManyObjects;
    uint64_t mostObjectsListedByMember() const;
    bool isListedByMember(ValueSetID) const;
    Vector<uint32_t> m_madeNodesOfStatement;
    ListWithRoomToFill<uint32_t> m_usesOfStatement;
    ListThatStaysInPlace<uint32_t> m_uses;
    bool m_knowsRecordsAskedFor { true };
    Vector<ValueFlowNodeSet> m_declaredMasksOfBodies;
    bool m_knowsNumbersOfUsers { true };
    // Filled behind the rounds.
    BitVector m_isStatementMadeByRuleAtRest;
    bool m_areRoundsOver { false };
    uint32_t m_numberOfNodesBeforeRound { 0 };
    uint32_t m_turnsOfRound { 0 };

    struct StepTimes {
        struct Step {
            ASCIILiteral name;
            Seconds wall;
            Seconds cpuOfThisThread;
            Seconds cpuOutsideParallelLoops;
            Seconds cpuOfAllThreads;
            BusyThreads::Times byBusyThreads { };
        };

        void begin();
        void didFinish(ASCIILiteral step);
        void dump() const;

        bool isOn { false };
        MonotonicTime startOfStep;
        Seconds cpuAtStartOfStep;
        Seconds cpuInParallelLoopsAtStartOfStep;
        Seconds cpuOfAllThreadsAtStartOfStep;
        BusyThreads::Times byBusyThreadsAtStartOfStep { };
        Vector<Step> steps;
        Seconds busyOnAllThreads;
        Seconds busiestThreadOfTurn;
        Seconds longestShareOfTurn;
    };
    StepTimes m_stepTimes;

    ValueFlowSharedBits m_isReached;
    ParkedBodies* m_parkedBodies { nullptr };
    ValueFlowSharedBits m_hasStatementOfBodyRun;
    uint64_t m_numberOfTurn { 0 };
    ValueFlowNodeID nodeAwaitedBy(ValueFlowStatementID, const ValueFlowStatement&) const;
    static constexpr uint32_t doesNotWait = UINT32_MAX;
    static constexpr uint32_t hasWaitedAndNotRun = UINT32_MAX - 1;
    bool m_statementsWait { false };
    struct StatementsWaitingForNodes {
        // One more than the statement, or 0.
        uint32_t exchangeFirst(ValueFlowNodeID node, uint32_t first)
        {
            Locker locker { lock };
            if (!first)
                return firstOfNode.take(node.index());
            auto added = firstOfNode.add(node.index(), first);
            return added.isNewEntry ? 0 : std::exchange(added.iterator->value, first);
        }

        Lock lock;
        HashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> firstOfNode WTF_GUARDED_BY_LOCK(lock);
    };
    static constexpr unsigned numberOfTablesOfStatementsWaiting = 64;
    StatementsWaitingForNodes& statementsWaitingFor(ValueFlowNodeID node) const LIFETIME_BOUND { return m_statementsWaitingForNodes[node.index() * 0x9e3779b1u >> 26]; }
    mutable Vector<StatementsWaitingForNodes> m_statementsWaitingForNodes;
    ListWithRoomToFill<uint32_t> m_nextStatementWaiting;
    Vector<ValueFlowReachWitness> m_reachedBy;
    Vector<RunsAndEdges> m_runsAndEdges;
    uint64_t m_numberOfRunsThatWereNotDueAndAdded { 0 };
    bool m_usesGeneralRulesOnly { false };
    AddedByGeneralRules m_addedByGeneralRules;
    Vector<ValueFlowStatementID> m_firstStatementsThatWereNotDueAndAdded;
    HashMap<uint64_t, uint32_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_positionOfLoggedEdge;
    Vector<LoggedEdge> m_edgesInOrderLogged;
    HashMap<uint32_t, Vector<ValueFlowNodeID, 1>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_sourcesOfRemovedEdges;
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
    Vector<ValueFlowStatementID> m_statementsThatRunOften;
    BitVector m_waitsForLaterRound;
    void takeBackStatementsThatRunOften();
    Vector<uint8_t> m_timesRunOfStatement;
    HashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_roundOfNextRun;
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

    ListWithRoomToFill<uint32_t> m_readersOfNode;
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
        ValueFlowNumberLists readersOfNodes;
        Vector<Reader> readersWaitingForSomething;
        ValueFlowPairSet knownToWaitForSomething;
        HashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> firstReaderWaitingForSomething;
    };
    static constexpr unsigned numberOfReaderShards = 64;
    static constexpr unsigned readerShardOf(ValueFlowNodeID node) { return node.index() / 16 % numberOfReaderShards; }
    std::array<ReaderShard, numberOfReaderShards> m_readerShards;
    struct Watch {
        ValueSetID within;
        uint32_t readers { 0 };
        uint32_t readersAskingLast { 0 };
        ValueFlowNodeID node;
    };
    struct WatchedNode {
        ValueFlowNodeID node;
        ValueSetID seen;
        CompactList<uint32_t> watches;
        CompactList<uint32_t> watchesOfManyObjects;
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
        bool isReaderOf(uint32_t watch, ValueFlowStatementID statement) const { return readersOfWatches.contains(watches[watch].readers, statement.index()) || readersOfWatches.contains(watches[watch].readersAskingLast, statement.index()); }

        ValueFlowNumberLists readersOfWatches;
        Vector<Watch> watches;
        static uint64_t hashOfPart(ValueFlowNodeID node, ValueSetID within) { return ValueFlowNumbersOfRecords::hashOf(static_cast<uint64_t>(node.index()) << 32 | within.index()); }
        std::optional<uint32_t> findWatchOf(ValueFlowNodeID node, ValueSetID within) const
        {
            return watchOfPart.find(hashOfPart(node, within), [&](uint32_t number) {
                return watches[number].node == node && watches[number].within == within;
            });
        }
        std::optional<uint32_t> findWatchedNode(ValueFlowNodeID node) const
        {
            return watchedNodeOfNode.find(ValueFlowNumbersOfRecords::hashOf(node.index()), [&](uint32_t number) {
                return watchedNodes[number].node == node;
            });
        }

        ValueFlowNumbersOfRecords watchOfPart;
        Vector<WatchedNode> watchedNodes;
        ValueFlowNumbersOfRecords watchedNodeOfNode;
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
    // A range of members owns whole words of the bits.
    static_assert(!(1024 % 64));
    Vector<uint64_t> m_hasSetWatchedForCoverOfMember;
    bool hasSetWatchedForCover(uint32_t member) const { return member / 64 < m_hasSetWatchedForCoverOfMember.size() && (m_hasSetWatchedForCoverOfMember[member / 64] >> member % 64 & 1); }
    std::array<ValueFlowNumbersByKey, numberOfRangesOfMembers> m_firstSetWatchedForCoverOfMember;
    std::array<Vector<ChainedWatch>, numberOfRangesOfMembers> m_setsWatchedForCoverOfMembers;
    ValueSetID m_seenByWatchersOfEscaped;
    ValueSetID m_seenByWatchersOfLost;
    BitVector m_isWatched;
    HashMap<uint32_t, Vector<ValueFlowStatementID>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_readersOfFunction;
    HashMap<uint32_t, Vector<ValueFlowNodeSet, 1>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_valuesShownToStatement;
    mutable ListWithRoomToFill<ValueSetID> m_rememberedByStatement;
    // By object: the object and what a walk up its chain of prototypes has seen above it. The chain of the object begins with that.
    ZeroFilledPages<std::atomic<uint32_t>> m_seenFromObjectUp;
    BitVector m_namesAskedFor;

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
    // 0 for what is no object.
    uint32_t layoutOrCalleeOf(ValueFlowObjectID) const;
    HashMap<uint32_t, bool, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_isTooLarge;
    BitVector m_isSaturated;

    Vector<Name> m_names;
    std::array<KindOfNames, mostKindsOfNames> m_kindsOfNames;
    static uint64_t hashOfName(ValueFlowName name) { return ValueFlowNumbersOfRecords::hashOf(static_cast<uint64_t>(name.kind) << 32 | name.payload); }
    std::optional<uint32_t> findNumberOfName(ValueFlowName name) const
    {
        return m_numberOfName.find(hashOfName(name), [&](uint32_t number) {
            return m_names[number].name.kind == name.kind && m_names[number].name.payload == name.payload;
        });
    }
    uint32_t ensureNumberOfName(ValueFlowName);
    ValueFlowNumbersOfRecords m_numberOfName;
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
        ValueFlowNumberMaps listOfMember;
        ValueFlowNumberLists lists;
    };
    struct Linked {
        struct LongList {
            NodeOfSet node;
            bool isRead { false };
            ListOfNodesOfSets* others { nullptr };
        };

        Vector<ValueFlowEdge> edges;
        uint64_t testsWhetherSetsMeet { 0 };
        Vector<ListOfNodesOfSets*> listsMostlyAtRest;
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
    // It starts over behind the last state, and every node forgets then.
    static constexpr uint8_t noStateOfCoverings = UINT8_MAX;
    uint8_t m_stateOfCoverings { 0 };
    mutable ListWithRoomToFill<uint8_t> m_coveringStateOfShownView;
    Vector<ObjectOutsideFamily> m_objectsOutsideFamilies;
    HashMap<uint32_t, ValueSetID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_closuresFollowedToConstruction;
    BitVector m_holdsObjectOutsideFamily;
    Vector<uint8_t> m_unknownSourceOfNode;

    Counters m_counters;
    Vector<Round> m_roundsDone;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
