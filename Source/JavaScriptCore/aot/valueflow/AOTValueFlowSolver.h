/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTValueFlowGraph.h"
#include "AOTValueFlowLog.h"
#include "AOTValueFlowRuleContext.h"
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/Seconds.h>

namespace JSC { namespace AOT { namespace Typed {

using ValueFlowRule = void (*)(const ValueFlowStatement&, ValueFlowRuleContext&);

// What the bodies give. Body i is function i; the last body is the host's, which is reached from the start.
struct ValueFlowSolverInput {
    // For the rules alone.
    const Program* program { nullptr };
    const ValueFlowUniverse* universe { nullptr };
    const ValueFlowProgramNodes* nodes { nullptr };

    // The node that holds whatever has reached native code, and the member that stands for all of that. With them the solver makes one statement of kind Escape, which is due from the start.
    ValueFlowNodeID escaped;
    ValueFlowObjectID unknownObject;
    // What that member stands for at a node of a body whose type lists every object inside it.
    ValueSetID objectsWithLayoutOfProgram;

    ValueFlowStandIn standIn() const { return { unknownObject, objectsWithLayoutOfProgram }; }

    uint32_t numberOfNodes { 0 };
    std::span<const ValueFlowEdge> edges;
    std::span<const ValueFlowNodeSet> masks;
    std::span<const ValueFlowStatement> statements;
    std::span<const ValueFlowOperand> operands;
    std::span<const ValueFlowEdge> edgesOnceReached;
    std::span<const ValueFlowNodeSet> seedsOnceReached;
    std::span<const FunctionID> functionsEnteredOnceReached;
    // One more than there are bodies.
    std::span<const ValueFlowBodyRange> ranges;

    // A solve whose rules log more edges than this stops without a result.
    uint64_t mostEdgesLogged { UINT64_MAX };
    // For each edge that a rule has logged the solver remembers the statement that logged it first. It costs 20 bytes an edge: for tests and reports.
    bool remembersWhoLoggedEachEdge { false };
};

// Why a function is reached: the statement that reached it, or the body that enters it as soon as that body is reached itself. Neither for the host's body.
struct ValueFlowReachWitness {
    ValueFlowStatementID statement;
    uint32_t enteringBody { UINT32_MAX };
    // The number of rounds that were finished when the function was reached.
    uint32_t round { 0 };
};

// Pairs of numbers, each kept once.
class ValueFlowPairSet {
public:
    bool add(uint32_t first, uint32_t second);
    size_t memoryInBytes() const { return m_slots.capacity() * sizeof(uint64_t); }

private:
    void grow();

    // One more than (first << 32 | second); zero for a free slot.
    Vector<uint64_t> m_slots;
    size_t m_size { 0 };
};

// The rounds. In a round every statement that is due runs on the frozen values, the logs are merged, and the values are brought up to date. A statement is due when something it read in its last run has moved.
// The solve ends with a round in which every statement runs and none adds anything.
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
        uint32_t sweeps { 0 };
        uint32_t evaluations { 0 };
        bool hasOrdered { false };
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
        // Every statement runs in every round. The results are the same.
        Full,
    };

    ValueFlowSolver(const ValueFlowSolverInput&, ValueSets&, ValueFlowRule, const ParallelLoops&, Rounds = Rounds::Due);
    ~ValueFlowSolver();

    // False if the solve has stopped at a limit: then nothing that it holds is a result.
    bool solve();

    const ValueSets& sets() const { return m_sets; }
    const ValueFlowGraph& graph() const { return m_graph; }
    ValueSetID valueOf(ValueFlowNodeID node) const { return m_graph.valueOf(node); }
    // After the solve: no rule runs for it and nothing flows on.
    void setSolvedValue(ValueFlowNodeID node, ValueSetID value) { m_graph.setSolvedValue(node, value); }
    bool isReached(FunctionID function) const { return m_isReached.quickGet(function.index()); }
    OptionSet<BuiltinUse> usesOf(BuiltinID) const;
    // After the solve.
    OptionSet<ObjectMutation> marksOf(ValueFlowObjectID) const;

    const ValueFlowSolverInput& input() const LIFETIME_BOUND { return m_input; }
    uint32_t numberOfStatements() const { return m_input.statements.size() + m_madeStatements.size(); }
    const ValueFlowStatement& statementAt(ValueFlowStatementID statement) const LIFETIME_BOUND
    {
        size_t index = statement.index();
        return index < m_input.statements.size() ? m_input.statements[index] : m_madeStatements[index - m_input.statements.size()].statement;
    }

    // Among the statements that reached the function in the turn in which it was reached, the one with the lowest number.
    ValueFlowReachWitness reachedBy(FunctionID function) const { return m_reachedBy[function.index()]; }
    // How the member has come into the node: the node first, then a predecessor that holds the member, and so on, on a shortest way, to a node that holds it for ever. Empty if the node does not hold it.
    Vector<ValueFlowNodeID> explain(ValueFlowNodeID, ValueFlowObjectID member) const;
    // None for an edge of a body, for one that the solver has made itself (from what is written to what is read, into a masked node, between groups of callees) and where the input did not ask for it.
    ValueFlowStatementID statementThatLogged(ValueFlowEdge) const;
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

    const Counters& counters() const { return m_counters; }
    std::span<const Round> rounds() const LIFETIME_BOUND { return m_roundsDone.span(); }
    size_t memoryInBytes() const;

private:
    friend class ValueFlowRuleContext;

    struct MadeNodes {
        ValueFlowNodeKey key;
        ValueFlowNodeID first;
        // The statement that was made with them, if any.
        ValueFlowStatementID statement;
        // The statements whose last run has asked for them. Without any, the nodes are at rest and their statement does not run: whoever asked for them asks for those of a bigger set now, which cover them.
        uint32_t numberOfUsers { 0 };
        // Nothing comes out of what is written through no object or handed to no callee.
        bool staysAtRest { false };
        bool hasBeenAtRest { false };
        uint64_t lastRunThatAsked { UINT64_MAX };
        // Of a node that reads or writes: whether it stands in the lists of its name.
        bool isListedUnderName { false };
        bool isListedUnderMembers { false };
        // The group of a big set of callees has no statement: it hands on to the groups of the parts of the set, which the groups of other sets share.
        uint32_t firstPart { 0 };
        uint32_t numberOfParts { 0 };
        // While somebody asks for it, it asks for its parts.
        uint32_t firstUseOfParts { UINT32_MAX };
    };

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
        Vector<NodeOfSet> ofManyObjects;
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

    struct Thread;

    // What a rule sees.
    std::optional<ValueFlowNodeID> findMadeNode(const ValueFlowNodeKey&) const;
    std::span<const ValueFlowOperand> operandsOf(const ValueFlowStatement& statement) const LIFETIME_BOUND { return m_input.operands.subspan(statement.firstOperand, statement.operandCount); }
    const SettledValues& settled() const { return m_settled; }

    void makeDue(ValueFlowStatementID);
    void makeStatementsDue(uint32_t first, uint32_t end);
    void makeEveryStatementDue();
    bool reach(uint32_t body, ValueFlowReachWitness);
    void takeReachedFunctions();
    void countRunsAndEdges();
    void rememberWhoLoggedTheEdges();
    bool runRules();
    void runDueStatements();
    bool merge();
    void numberMadeNodes();
    ValueFlowNodeID makeNodes(const ValueFlowNodeKey&);
    ValueFlowNodeID ensureConstant(ValueSetID);
    void handOnToGroupsOfParts(uint32_t madeNodes, std::span<const ValueSetID> parts);
    void joinNodeUnderKind(uint32_t madeNodes);
    void listToLink(const MadeNodes&);
    void linkNamedNodes();
    Name& ensureName(ValueFlowName);
    void link(const NodeOfSet&, bool isRead, NodesOfName& others, uint32_t numberOfName);
    void insert(const NodeOfSet&, bool isRead, NodesOfName&, uint32_t numberOfName);
    void noteInputs();
    void noteNodesAskedFor();
    uint32_t acquire(uint32_t madeNodes, uint32_t firstUse);
    void release(uint32_t firstUse);
    void makeReadersDue(std::span<const ValueFlowNodeID> moved);
    // What a rule gets to see of a node's value.
    ValueSetID valueForReadersOf(ValueFlowNodeID);
    void spreadMarks();

    const ValueFlowSolverInput& m_input;
    ValueSets& m_sets;
    const ValueFlowRule m_rule;
    const ParallelLoops& m_loops;
    const Rounds m_rounds;
    SettledValues m_settled;

    Vector<std::unique_ptr<ValueSetMaker>> m_makers;
    ValueFlowGraph m_graph;
    Vector<std::unique_ptr<Thread>> m_threads;

    Vector<MadeNodes> m_madeNodes;
    HashedIndex m_madeNodesByKey;
    Vector<MadeStatement> m_madeStatements;
    // By the number of a made node less the number of nodes of the bodies.
    Vector<uint32_t> m_madeNodesOfNode;
    Vector<uint32_t> m_partsOfMadeNodes;
    Vector<uint32_t> m_madeNodesOfStatement;
    Vector<uint32_t> m_firstUseOfStatement;
    Vector<Use> m_uses;
    uint32_t m_firstFreeUse { UINT32_MAX };
    uint64_t m_numberOfRunsNoted { 0 };
    uint32_t m_numberOfNodesBeforeRound { 0 };
    uint32_t m_turnsOfRound { 0 };

    BitVector m_isReached;
    Vector<ValueFlowReachWitness> m_reachedBy;
    Vector<RunsAndEdges> m_runsAndEdges;
    HashMap<uint64_t, ValueFlowStatementID, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_statementOfEdge;
    Vector<uint32_t> m_bodiesReachedInRound;
    size_t m_numberOfBodiesWhoseStatementsAreDue { 0 };

    BitVector m_isDue;
    Vector<ValueFlowStatementID> m_due;

    Vector<uint32_t> m_firstReaderOfNode;
    // Of a node that has readers: each of them has seen all of this, but for what has escaped since.
    Vector<ValueSetID> m_seenByReadersOfNode;
    ValueSetID m_unknownObjectAlone;
    bool m_isFullRound { false };
    Vector<Reader> m_readers;
    ValueFlowPairSet m_knownReaders;
    HashMap<uint32_t, Vector<ValueFlowStatementID>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_readersOfFunction;

    Vector<Name> m_names;
    std::array<KindOfNames, mostKindsOfNames> m_kindsOfNames;
    HashMap<uint64_t, uint32_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_numberOfName;
    // By (number of the name * 2 + whether it reads, object): the nodes of few objects that hold the object, as a chain.
    HashMap<uint64_t, uint32_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_firstNodeOfMember;
    struct ChainedNode {
        ValueFlowNodeID node;
        uint32_t next { UINT32_MAX };
    };
    Vector<ChainedNode> m_nodesOfMembers;
    // Made or woken in this round.
    struct NodeToLink {
        NodeOfSet node;
        ValueFlowName name;
        bool standsForKind { false };
    };
    Vector<NodeToLink> m_readToLink;
    Vector<NodeToLink> m_writtenToLink;

    Vector<ValueFlowEdge> m_edgesOfRound;
    Vector<OptionSet<ObjectMutation>> m_marksOfObject;
    Vector<OptionSet<BuiltinUse>> m_usesOfBuiltin;

    Counters m_counters;
    Vector<Round> m_roundsDone;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
