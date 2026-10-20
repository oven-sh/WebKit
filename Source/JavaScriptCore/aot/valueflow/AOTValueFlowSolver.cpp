/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowSolver.h"

#if ENABLE(AOT)

#include <algorithm>
#include <tuple>
#include <wtf/MonotonicTime.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t none = UINT32_MAX;
static constexpr uint32_t mostObjectsListedByMember = 8;
static constexpr uint32_t mostObjectsLookedUpOneByOne = 64;

bool ValueFlowPairSet::add(uint32_t first, uint32_t second)
{
    if ((m_size + 1) * 3 > m_slots.size() * 2)
        grow();
    uint64_t key = (static_cast<uint64_t>(first) << 32 | second) + 1;
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

void ValueFlowPairSet::grow()
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

struct ValueFlowSolver::Thread {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Thread);

    Thread(const ValueFlowSolver& solver, ValueSetMaker& maker)
        : context(solver, log, maker)
    {
    }

    ValueFlowNodeID numberInGraph(ValueFlowNodeID node) const
    {
        return ValueFlowRoundLog::isNumberedByLog(node) ? ValueFlowNodeID(numberOfMadeNode[node.index() & ~ValueFlowRoundLog::numberedByLog]) : node;
    }

    ValueFlowRoundLog log;
    ValueFlowRuleContext context;
    // By the log's number of a node: its number in the graph.
    Vector<uint32_t> numberOfMadeNode;
    // How much of the log the turns before this one have dealt with.
    size_t numberOfReachedTaken { 0 };
    size_t numberOfMadeTaken { 0 };
    size_t numberOfRunsTaken { 0 };
    uint32_t numberOfNodesAskedForTaken { 0 };
};

static Vector<std::unique_ptr<ValueSetMaker>> makeMakers(ValueSets& sets, unsigned count)
{
    return Vector<std::unique_ptr<ValueSetMaker>>(count, [&](size_t) {
        return makeUnique<ValueSetMaker>(sets);
    });
}

ValueFlowSolver::ValueFlowSolver(const ValueFlowSolverInput& input, ValueSets& sets, ValueFlowRule rule, const ParallelLoops& loops, Rounds rounds)
    : m_input(input)
    , m_sets(sets)
    , m_rule(rule)
    , m_loops(loops)
    , m_rounds(rounds)
    , m_makers(makeMakers(sets, loops.width()))
    , m_graph(loops, m_makers.span())
{
    m_graph.setStandIn(m_input.standIn());
    if (m_input.unknownObject)
        m_unknownObjectAlone = m_makers[0]->of(m_input.unknownObject);
    for (unsigned written = 0; written < mostKindsOfNames; ++written) {
        for (unsigned read = 0; read < mostKindsOfNames; ++read) {
            if (!mayBeTheSameName(static_cast<ValueFlowNameKind>(written), static_cast<ValueFlowNameKind>(read)))
                continue;
            m_kindsOfNames[written].readKindsThatMayFindIt |= 1ull << read;
            m_kindsOfNames[read].writtenKindsItMayFind |= 1ull << written;
        }
    }
    for (const std::unique_ptr<ValueSetMaker>& maker : m_makers)
        m_threads.append(makeUnique<Thread>(*this, *maker));
    m_isReached.ensureSize(m_input.ranges.size());
    m_reachedBy.grow(m_input.ranges.size());
    m_isDue.ensureSize(m_input.statements.size());
}

ValueFlowSolver::~ValueFlowSolver() = default;

std::optional<ValueFlowNodeID> ValueFlowSolver::findMadeNode(const ValueFlowNodeKey& key) const
{
    std::optional<uint32_t> position = m_madeNodesByKey.find(key.hash(), [&](uint32_t candidate) {
        return m_madeNodes[candidate].key == key;
    });
    if (!position)
        return std::nullopt;
    return m_madeNodes[*position].first;
}

bool ValueFlowSolver::isAtRest(ValueFlowStatementID statement) const
{
    if (statement.index() < m_input.statements.size())
        return false;
    uint32_t madeNodes = m_madeNodesOfStatement[statement.index() - m_input.statements.size()];
    return madeNodes != none && !m_madeNodes[madeNodes].numberOfUsers;
}

void ValueFlowSolver::makeDue(ValueFlowStatementID statement)
{
    if (isAtRest(statement) || m_isDue.set(statement.index()))
        return;
    m_due.append(statement);
}

void ValueFlowSolver::makeStatementsDue(uint32_t first, uint32_t end)
{
    for (uint32_t statement = first; statement < end; ++statement)
        makeDue(ValueFlowStatementID(statement));
}

void ValueFlowSolver::makeEveryStatementDue()
{
    for (uint32_t body = 0; body + 1 < m_input.ranges.size(); ++body)
        makeStatementsDue(m_input.ranges[body].statement, m_isReached.quickGet(body) ? m_input.ranges[body + 1].statement : m_input.ranges[body].statementLeavingBody);
    makeStatementsDue(m_input.statements.size(), numberOfStatements());
}

bool ValueFlowSolver::reach(uint32_t firstBody, ValueFlowReachWitness witness)
{
    if (m_isReached.quickGet(firstBody))
        return false;
    Vector<uint32_t, 16> bodies { firstBody };
    m_isReached.quickSet(firstBody);
    witness.round = m_counters.rounds;
    m_reachedBy[firstBody] = witness;
    while (!bodies.isEmpty()) {
        uint32_t body = bodies.takeLast();
        ++m_counters.functionsReached;
        m_bodiesReachedInRound.append(body);
        makeStatementsDue(m_input.ranges[body].statementLeavingBody, m_input.ranges[body + 1].statement);
        auto readers = m_readersOfFunction.find(body);
        if (readers != m_readersOfFunction.end()) {
            for (ValueFlowStatementID statement : readers->value)
                makeDue(statement);
        }
        for (FunctionID entered : m_input.functionsEnteredOnceReached.subspan(m_input.ranges[body].functionEntered, m_input.ranges[body + 1].functionEntered - m_input.ranges[body].functionEntered)) {
            if (!m_isReached.quickSet(entered.index())) {
                m_reachedBy[entered.index()] = ValueFlowReachWitness { { }, body, m_counters.rounds };
                bodies.append(entered.index());
            }
        }
    }
    return true;
}

void ValueFlowSolver::runDueStatements()
{
    constexpr size_t statementsPerShare = 32;
    Vector<ValueFlowStatementID> due = std::exchange(m_due, Vector<ValueFlowStatementID>());
    for (ValueFlowStatementID statement : due)
        m_isDue.quickClear(statement.index());
    due.removeAllMatching([&](ValueFlowStatementID statement) {
        return isAtRest(statement);
    });
    m_counters.statementRuns += due.size();
    uint64_t edgesLogged = m_counters.edgesLogged;
    for (const std::unique_ptr<Thread>& thread : m_threads)
        edgesLogged += thread->log.edges.size();
    for (const std::unique_ptr<Thread>& thread : m_threads)
        thread->log.mostEdges = thread->log.edges.size() + (m_input.mostEdgesLogged - std::min(edgesLogged, m_input.mostEdgesLogged));
    forEachIndexInParallel(m_loops, (due.size() + statementsPerShare - 1) / statementsPerShare, [&](unsigned share, unsigned worker) {
        ValueFlowRuleContext& context = m_threads[worker]->context;
        for (ValueFlowStatementID statement : due.subspan(share * statementsPerShare, std::min(statementsPerShare, due.size() - share * statementsPerShare))) {
            if (m_threads[worker]->log.hasDroppedEdges)
                return;
            context.begin(statement);
            m_rule(statementAt(statement), context);
            context.end();
        }
    });
}

void ValueFlowSolver::takeReachedFunctions()
{
    struct Reached {
        FunctionID function;
        ValueFlowStatementID statement;
    };
    Vector<Reached> reached;
    for (const std::unique_ptr<Thread>& thread : m_threads) {
        size_t next = thread->numberOfReachedTaken;
        for (const ValueFlowRoundLog::Run& run : thread->log.runs.subspan(thread->numberOfRunsTaken)) {
            for (; next < run.endOfReached; ++next)
                reached.append(Reached { thread->log.reached[next], run.statement });
        }
        thread->numberOfReachedTaken = next;
    }
    std::ranges::sort(reached, [](const Reached& first, const Reached& second) {
        return std::tuple { first.function, first.statement } < std::tuple { second.function, second.statement };
    });
    for (const Reached& entry : reached)
        reach(entry.function.index(), ValueFlowReachWitness { entry.statement, none });
}

// A function that a statement reaches and a statement that is made with a node run in the same round, in its next turn, on the same frozen values.
bool ValueFlowSolver::runRules()
{
    for (m_turnsOfRound = 0; !m_due.isEmpty(); ++m_turnsOfRound) {
        runDueStatements();
        uint64_t edgesLogged = m_counters.edgesLogged;
        for (const std::unique_ptr<Thread>& thread : m_threads) {
            edgesLogged += thread->log.edges.size();
            if (thread->log.hasDroppedEdges || edgesLogged > m_input.mostEdgesLogged)
                return false;
        }
        takeReachedFunctions();
        numberMadeNodes();
        noteNodesAskedFor();
    }
    return true;
}

ValueFlowNodeID ValueFlowSolver::makeNodes(const ValueFlowNodeKey& key)
{
    ValueFlowNodeID first = m_graph.addNodes(key.numberOfNodes());
    m_madeNodesByKey.add(key.hash(), m_madeNodes.size());
    m_madeNodesOfNode.insertFill(m_madeNodesOfNode.size(), m_madeNodes.size(), key.numberOfNodes());
    m_madeNodes.append(MadeNodes { key, first, { }, 0 });
    if (key.set == ValueSets::nothing && (key.kind == ValueFlowMadeNodeKind::Written || key.kind == ValueFlowMadeNodeKind::CallGroup)) {
        m_madeNodes.last().staysAtRest = true;
        m_graph.putToRest(first, key.numberOfNodes());
        return first;
    }

    auto makeStatement = [&](ValueFlowStatementKind kind, ValueFlowNodeID result, ValueFlowNodeID thrown) {
        ValueFlowStatement statement;
        statement.kind = kind;
        statement.result = result;
        statement.thrown = thrown;
        m_madeStatements.append(MadeStatement { statement, key });
        m_madeNodesOfStatement.append(m_madeNodes.size() - 1);
        m_madeNodes.last().statement = ValueFlowStatementID(numberOfStatements() - 1);
        m_isDue.ensureSize(numberOfStatements());
    };

    switch (key.kind) {
    case ValueFlowMadeNodeKind::Constant:
        m_graph.makeConstant(first, key.set);
        break;
    case ValueFlowMadeNodeKind::Read:
    case ValueFlowMadeNodeKind::Written:
        listToLink(m_madeNodes.last());
        joinNodeUnderKind(m_madeNodes.size() - 1);
        break;
    case ValueFlowMadeNodeKind::ReadUnderKind:
    case ValueFlowMadeNodeKind::WrittenUnderKind:
        listToLink(m_madeNodes.last());
        break;
    case ValueFlowMadeNodeKind::Lookup:
        makeStatement(ValueFlowStatementKind::Lookup, first, { });
        break;
    case ValueFlowMadeNodeKind::CallGroup: {
        Vector<ValueSetID> parts;
        m_makers[0]->appendParts(key.set, parts);
        if (!parts.isEmpty()) {
            handOnToGroupsOfParts(m_madeNodes.size() - 1, parts.span());
            break;
        }
        ValueFlowCallGroup group = callGroupAt(first);
        makeStatement(ValueFlowStatementKind::CallGroup, group.result, group.thrown);
        break;
    }
    case ValueFlowMadeNodeKind::Hub:
        makeStatement(ValueFlowStatementKind::Behavior, first, { });
        break;
    case ValueFlowMadeNodeKind::Masked:
        m_graph.setMask(first, key.set, ValueFlowGraph::MaskKind::Plain);
        m_graph.startWithValueOf(first, key.maskedNode());
        m_edgesOfRound.append(ValueFlowEdge { key.maskedNode(), first });
        break;
    case ValueFlowMadeNodeKind::Part:
        m_graph.setMask(first, key.set, ValueFlowGraph::MaskKind::Plain);
        m_graph.startWithValueOf(first, key.maskedNode());
        m_edgesOfRound.append(ValueFlowEdge { key.maskedNode(), first });
        break;
    case ValueFlowMadeNodeKind::Marked:
        break;
    }
    return first;
}

void ValueFlowSolver::listToLink(const MadeNodes& madeNodes)
{
    const ValueFlowNodeKey& key = madeNodes.key;
    bool standsForKind = key.kind == ValueFlowMadeNodeKind::ReadUnderKind || key.kind == ValueFlowMadeNodeKind::WrittenUnderKind;
    bool isRead = key.kind == ValueFlowMadeNodeKind::Read || key.kind == ValueFlowMadeNodeKind::ReadUnderKind;
    (isRead ? m_readToLink : m_writtenToLink).append(NodeToLink { { key.set, madeNodes.first }, key.name(), standsForKind });
}

// While somebody asks for a node that reads or writes, it asks for the node that stands for its kind.
void ValueFlowSolver::joinNodeUnderKind(uint32_t number)
{
    ValueFlowNodeKey key = m_madeNodes[number].key;
    ValueFlowNodeID first = m_madeNodes[number].first;
    bool isRead = key.kind == ValueFlowMadeNodeKind::Read;
    const KindOfNames& kind = m_kindsOfNames[static_cast<unsigned>(key.name().kind)];
    if (!(isRead ? kind.writtenKindsItMayFind : kind.readKindsThatMayFindIt))
        return;

    ValueFlowNodeKey keyUnderKind = ValueFlowNodeKey::underKind(isRead ? ValueFlowMadeNodeKind::ReadUnderKind : ValueFlowMadeNodeKind::WrittenUnderKind, key.set, key.name().kind);
    std::optional<ValueFlowNodeID> existing = findMadeNode(keyUnderKind);
    ValueFlowNodeID underKind = existing ? *existing : makeNodes(keyUnderKind);
    m_edgesOfRound.append(isRead ? ValueFlowEdge { underKind, first } : ValueFlowEdge { first, underKind });
    m_madeNodes[number].firstPart = m_partsOfMadeNodes.size();
    m_madeNodes[number].numberOfParts = 1;
    m_partsOfMadeNodes.append(m_madeNodesOfNode[underKind.index() - m_input.numberOfNodes]);
}

void ValueFlowSolver::handOnToGroupsOfParts(uint32_t number, std::span<const ValueSetID> parts)
{
    ValueFlowNodeID first = m_madeNodes[number].first;
    ValueFlowCallGroup group = callGroupAt(first);
    Vector<uint32_t, 128> numbersOfParts;
    for (ValueSetID part : parts) {
        ValueFlowNodeKey key = ValueFlowNodeKey::callGroup(part, m_madeNodes[number].key.callKind());
        std::optional<ValueFlowNodeID> existing = findMadeNode(key);
        ValueFlowNodeID firstOfPart = existing ? *existing : makeNodes(key);
        numbersOfParts.append(m_madeNodesOfNode[firstOfPart.index() - m_input.numberOfNodes]);
        for (unsigned offset = 0; offset < ValueFlowNodeKey::numberOfNodesOfCallGroup; ++offset) {
            ValueFlowEdge down { ValueFlowNodeID(first.index() + offset), ValueFlowNodeID(firstOfPart.index() + offset) };
            bool comesBack = down.from == group.result || down.from == group.thrown;
            m_edgesOfRound.append(comesBack ? ValueFlowEdge { down.to, down.from } : down);
        }
    }
    m_madeNodes[number].firstPart = m_partsOfMadeNodes.size();
    m_madeNodes[number].numberOfParts = numbersOfParts.size();
    m_partsOfMadeNodes.appendVector(numbersOfParts);
}

ValueFlowNodeID ValueFlowSolver::ensureConstant(ValueSetID set)
{
    ValueFlowNodeKey key = ValueFlowNodeKey::constant(set);
    if (std::optional<ValueFlowNodeID> existing = findMadeNode(key))
        return *existing;
    return makeNodes(key);
}

// The nodes that the logs have made get their numbers in an order that depends on their keys alone, and a set counts in it by its members, not by its number.
void ValueFlowSolver::numberMadeNodes()
{
    struct Candidate {
        ValueFlowNodeKey key;
        uint64_t hashOfMembers;
        uint32_t thread;
        uint32_t position;
    };

    Vector<Vector<uint32_t>> waiting(m_threads.size());
    for (unsigned number = 0; number < m_threads.size(); ++number) {
        Thread& thread = *m_threads[number];
        thread.numberOfMadeNode.insertFill(thread.numberOfMadeNode.size(), none, thread.log.numberOfMadeNodes - thread.numberOfMadeNode.size());
        for (size_t position = thread.numberOfMadeTaken; position < thread.log.made.size(); ++position)
            waiting[number].append(position);
        thread.numberOfMadeTaken = thread.log.made.size();
    }

    Vector<Candidate> candidates;
    for (;;) {
        // The key of a masked node names another node, which has to have its number first.
        candidates.shrink(0);
        for (unsigned thread = 0; thread < m_threads.size(); ++thread) {
            waiting[thread].removeAllMatching([&](uint32_t position) {
                ValueFlowNodeKey key = m_threads[thread]->log.made[position].key;
                if (key.kind == ValueFlowMadeNodeKind::Masked || key.kind == ValueFlowMadeNodeKind::Part) {
                    ValueFlowNodeID node = m_threads[thread]->numberInGraph(key.maskedNode());
                    if (!node)
                        return false;
                    key.payload = node.index();
                }
                candidates.append(Candidate { key, key.set ? m_sets.hashOfMembers(key.set) : 0, thread, position });
                return true;
            });
        }
        if (candidates.isEmpty())
            break;

        auto orderOf = [](const Candidate& candidate) {
            return std::tuple { candidate.key.kind, candidate.key.detail, candidate.key.payload, candidate.hashOfMembers, candidate.key.set.index() };
        };
        std::ranges::sort(candidates, { }, orderOf);
        ValueFlowNodeID first;
        for (size_t index = 0; index < candidates.size(); ++index) {
            const Candidate& candidate = candidates[index];
            if (!index || candidates[index - 1].key != candidate.key) {
                std::optional<ValueFlowNodeID> existing = findMadeNode(candidate.key);
                first = existing ? *existing : makeNodes(candidate.key);
            }
            Thread& thread = *m_threads[candidate.thread];
            uint32_t firstInLog = thread.log.made[candidate.position].first;
            for (unsigned offset = 0; offset < candidate.key.numberOfNodes(); ++offset)
                thread.numberOfMadeNode[firstInLog + offset] = first.index() + offset;
        }
    }
    for (const Vector<uint32_t>& left : waiting)
        RELEASE_ASSERT(left.isEmpty());
}

auto ValueFlowSolver::ensureName(ValueFlowName name) -> Name&
{
    uint64_t key = static_cast<uint64_t>(name.kind) << 32 | name.payload;
    uint32_t number = m_numberOfName.ensure(key, [&] {
        m_names.append(Name { name, { }, { } });
        return static_cast<uint32_t>(m_names.size() - 1);
    }).iterator->value;
    return m_names[number];
}

static bool isListedByMember(const ValueSets& sets, ValueSetID set)
{
    return !ValueSets::isComplement(set) && sets.size(set) <= mostObjectsListedByMember;
}

// A node at rest is passed over: it is linked when it wakes.
void ValueFlowSolver::link(const NodeOfSet& node, bool isRead, NodesOfName& others, uint32_t numberOfName)
{
    auto connect = [&](ValueFlowNodeID other) {
        if (!m_graph.isAtRest(other))
            m_edgesOfRound.append(isRead ? ValueFlowEdge { other, node.node } : ValueFlowEdge { node.node, other });
    };
    auto connectWhereSetsMeet = [&](Vector<NodeOfSet>& list) {
        size_t numberAtRest = 0;
        for (const NodeOfSet& other : list) {
            if (m_graph.isAtRest(other.node)) {
                ++numberAtRest;
                continue;
            }
            ++m_counters.testsWhetherSetsMeet;
            if (m_sets.meet(node.set, other.set))
                connect(other.node);
        }
        if (numberAtRest * 2 <= list.size())
            return;
        list.removeAllMatching([&](const NodeOfSet& other) {
            if (!m_graph.isAtRest(other.node))
                return false;
            m_madeNodes[m_madeNodesOfNode[other.node.index() - m_input.numberOfNodes]].isListedUnderName = false;
            return true;
        });
    };

    connectWhereSetsMeet(others.ofManyObjects);
    if (ValueSets::isComplement(node.set) || m_sets.size(node.set) > mostObjectsLookedUpOneByOne) {
        connectWhereSetsMeet(others.ofFewObjects);
        return;
    }
    uint64_t table = static_cast<uint64_t>(numberOfName * 2 + !isRead) << 32;
    m_sets.forEachMember(node.set, [&](ValueFlowObjectID member) {
        auto first = m_firstNodeOfMember.find(table | member.index());
        if (first == m_firstNodeOfMember.end())
            return;
        for (uint32_t entry = first->value; entry != none; entry = m_nodesOfMembers[entry].next)
            connect(m_nodesOfMembers[entry].node);
    });
}

void ValueFlowSolver::insert(const NodeOfSet& node, bool isRead, NodesOfName& nodes, uint32_t numberOfName)
{
    MadeNodes& madeNodes = m_madeNodes[m_madeNodesOfNode[node.node.index() - m_input.numberOfNodes]];
    if (std::exchange(madeNodes.isListedUnderName, true))
        return;
    if (!isListedByMember(m_sets, node.set)) {
        nodes.ofManyObjects.append(node);
        return;
    }
    nodes.ofFewObjects.append(node);
    if (std::exchange(madeNodes.isListedUnderMembers, true))
        return;
    uint64_t table = static_cast<uint64_t>(numberOfName * 2 + isRead) << 32;
    m_sets.forEachMember(node.set, [&](ValueFlowObjectID member) {
        uint32_t& first = m_firstNodeOfMember.add(table | member.index(), none).iterator->value;
        m_nodesOfMembers.append(ChainedNode { node.node, first });
        first = m_nodesOfMembers.size() - 1;
    });
}

// What is written through a set reaches what is read under a name that may be the same through every set that shares an object with it. Each pair of nodes is looked at once, when the later of the two is made.
void ValueFlowSolver::linkNamedNodes()
{
    for (bool isRead : { false, true }) {
        for (const NodeToLink& toLink : isRead ? m_readToLink : m_writtenToLink) {
            if (m_graph.isAtRest(toLink.node.node))
                continue;
            if (!toLink.standsForKind) {
                uint32_t number = static_cast<uint32_t>(&ensureName(toLink.name) - m_names.begin());
                Name& name = m_names[number];
                link(toLink.node, isRead, isRead ? name.written : name.read, number);
                insert(toLink.node, isRead, isRead ? name.read : name.written, number);
                continue;
            }
            unsigned kind = static_cast<unsigned>(toLink.name.kind);
            for (uint64_t others = isRead ? m_kindsOfNames[kind].writtenKindsItMayFind : m_kindsOfNames[kind].readKindsThatMayFindIt; others; others &= others - 1) {
                unsigned other = std::countr_zero(others);
                link(toLink.node, isRead, isRead ? m_kindsOfNames[other].written : m_kindsOfNames[other].read, numberOfFirstKindAmongNames + other);
            }
            insert(toLink.node, isRead, isRead ? m_kindsOfNames[kind].read : m_kindsOfNames[kind].written, numberOfFirstKindAmongNames + kind);
        }
    }
    m_writtenToLink.shrink(0);
    m_readToLink.shrink(0);
}

void ValueFlowSolver::noteInputs()
{
    m_firstReaderOfNode.insertFill(m_firstReaderOfNode.size(), none, m_graph.numberOfNodes() - m_firstReaderOfNode.size());
    m_seenByReadersOfNode.insertFill(m_seenByReadersOfNode.size(), ValueSets::nothing, m_graph.numberOfNodes() - m_seenByReadersOfNode.size());
    for (const std::unique_ptr<Thread>& thread : m_threads) {
        const ValueFlowRoundLog& log = thread->log;
        uint32_t nodeRead = 0;
        uint32_t functionAsked = 0;
        for (const ValueFlowRoundLog::Run& run : log.runs) {
            for (; nodeRead < run.endOfNodesRead; ++nodeRead) {
                ValueFlowNodeID node = thread->numberInGraph(log.nodesRead[nodeRead]);
                if (!m_knownReaders.add(node.index(), run.statement.index()))
                    continue;
                if (m_firstReaderOfNode[node.index()] == none)
                    m_seenByReadersOfNode[node.index()] = valueForReadersOf(node);
                m_readers.append(Reader { run.statement, m_firstReaderOfNode[node.index()] });
                m_firstReaderOfNode[node.index()] = m_readers.size() - 1;
            }
            for (; functionAsked < run.endOfFunctionsAsked; ++functionAsked) {
                FunctionID function = log.functionsAsked[functionAsked];
                if (m_isReached.quickGet(function.index()))
                    continue;
                Vector<ValueFlowStatementID>& readers = m_readersOfFunction.add(function.index(), Vector<ValueFlowStatementID>()).iterator->value;
                if (!readers.contains(run.statement))
                    readers.append(run.statement);
            }
        }
    }
}

// Answers the list of uses with one more in front.
uint32_t ValueFlowSolver::acquire(uint32_t number, uint32_t firstUse)
{
    MadeNodes& madeNodes = m_madeNodes[number];
    if (!madeNodes.numberOfUsers++) {
        if (madeNodes.hasBeenAtRest) {
            --m_counters.madeNodesAtRest;
            m_graph.wake(madeNodes.first, madeNodes.key.numberOfNodes());
            // Nothing was linked to it while it was at rest.
            switch (madeNodes.key.kind) {
            case ValueFlowMadeNodeKind::Read:
            case ValueFlowMadeNodeKind::Written:
            case ValueFlowMadeNodeKind::ReadUnderKind:
            case ValueFlowMadeNodeKind::WrittenUnderKind:
                listToLink(madeNodes);
                break;
            default:
                break;
            }
        }
        if (madeNodes.statement)
            makeDue(madeNodes.statement);
        for (uint32_t part : m_partsOfMadeNodes.subspan(madeNodes.firstPart, madeNodes.numberOfParts))
            madeNodes.firstUseOfParts = acquire(part, madeNodes.firstUseOfParts);
    }
    uint32_t use = m_firstFreeUse;
    if (use == none) {
        use = m_uses.size();
        m_uses.append(Use { });
    } else
        m_firstFreeUse = m_uses[use].next;
    m_uses[use] = Use { number, firstUse };
    return use;
}

void ValueFlowSolver::release(uint32_t firstUse)
{
    Vector<uint32_t, 8> lists { firstUse };
    while (!lists.isEmpty()) {
        for (uint32_t use = lists.takeLast(); use != none;) {
            MadeNodes& madeNodes = m_madeNodes[m_uses[use].madeNodes];
            if (!--madeNodes.numberOfUsers) {
                ++m_counters.madeNodesAtRest;
                madeNodes.hasBeenAtRest = true;
                m_graph.putToRest(madeNodes.first, madeNodes.key.numberOfNodes());
                // What its statement has asked for is no longer asked for by it.
                if (madeNodes.statement)
                    lists.append(std::exchange(m_firstUseOfStatement[madeNodes.statement.index()], none));
                lists.append(std::exchange(madeNodes.firstUseOfParts, none));
            }
            uint32_t next = m_uses[use].next;
            m_uses[use].next = std::exchange(m_firstFreeUse, use);
            use = next;
        }
    }
}

// After each turn, in which a statement runs once at the most: its last run says what the statement asks for.
void ValueFlowSolver::noteNodesAskedFor()
{
    m_firstUseOfStatement.insertFill(m_firstUseOfStatement.size(), none, numberOfStatements() - m_firstUseOfStatement.size());
    Vector<uint32_t, 16> askedFor;
    Vector<uint32_t> usesBefore;
    for (const std::unique_ptr<Thread>& owned : m_threads) {
        Thread& thread = *owned;
        for (; thread.numberOfRunsTaken < thread.log.runs.size(); ++thread.numberOfRunsTaken) {
            const ValueFlowRoundLog::Run& run = thread.log.runs[thread.numberOfRunsTaken];
            askedFor.shrink(0);
            for (; thread.numberOfNodesAskedForTaken < run.endOfNodesAskedFor; ++thread.numberOfNodesAskedForTaken) {
                uint32_t madeNodes = m_madeNodesOfNode[thread.numberInGraph(thread.log.nodesAskedFor[thread.numberOfNodesAskedForTaken]).index() - m_input.numberOfNodes];
                MadeNodes& asked = m_madeNodes[madeNodes];
                // That a statement asks for what it was made with keeps nothing awake.
                if (asked.staysAtRest || asked.statement == run.statement || asked.lastRunThatAsked == m_numberOfRunsNoted)
                    continue;
                asked.lastRunThatAsked = m_numberOfRunsNoted;
                askedFor.append(madeNodes);
            }
            ++m_numberOfRunsNoted;
            usesBefore.append(std::exchange(m_firstUseOfStatement[run.statement.index()], none));
            for (uint32_t number : askedFor)
                m_firstUseOfStatement[run.statement.index()] = acquire(number, m_firstUseOfStatement[run.statement.index()]);
        }
    }
    // Afterwards, so that what one statement lets go of and another one takes up does not go to rest in between, whichever of the two comes first.
    for (uint32_t before : usesBefore)
        release(before);
}

void ValueFlowSolver::countRunsAndEdges()
{
    m_runsAndEdges.grow(numberOfOpcodes + 256);
    for (const std::unique_ptr<Thread>& thread : m_threads) {
        uint32_t edge = 0;
        uint32_t reached = 0;
        uint32_t made = 0;
        for (const ValueFlowRoundLog::Run& run : thread->log.runs) {
            const ValueFlowStatement& statement = statementAt(run.statement);
            unsigned index = statement.kind == ValueFlowStatementKind::Operation ? static_cast<unsigned>(statement.opcode) : numberOfOpcodes + static_cast<unsigned>(statement.kind);
            ++m_runsAndEdges[index].runs;
            bool hasReached = std::exchange(reached, run.endOfReached) != run.endOfReached;
            bool hasMade = std::exchange(made, run.endOfMade) != run.endOfMade;
            bool hasAdded = hasReached || hasMade || edge != run.endOfEdges;
            m_runsAndEdges[index].runsThatAddedNothing += !hasAdded;
            m_runsAndEdges[index].runsThatWereNotDueAndAdded += hasAdded && m_isFullRound;
            m_runsAndEdges[index].connections += run.numberOfConnections;
            m_runsAndEdges[index].edgesLogged += run.endOfEdges - std::exchange(edge, run.endOfEdges);
        }
    }
}

static uint64_t numberOf(ValueFlowEdge edge)
{
    return static_cast<uint64_t>(edge.from.index()) << 32 | edge.to.index();
}

void ValueFlowSolver::rememberWhoLoggedTheEdges()
{
    struct Logged {
        uint64_t edge;
        ValueFlowStatementID statement;
    };
    Vector<Logged> logged;
    for (const std::unique_ptr<Thread>& thread : m_threads) {
        uint32_t edge = 0;
        for (const ValueFlowRoundLog::Run& run : thread->log.runs) {
            for (; edge < run.endOfEdges; ++edge)
                logged.append(Logged { numberOf(ValueFlowEdge { thread->numberInGraph(thread->log.edges[edge].from), thread->numberInGraph(thread->log.edges[edge].to) }), run.statement });
        }
    }
    std::ranges::sort(logged, [](const Logged& first, const Logged& second) {
        return std::tuple { first.edge, first.statement } < std::tuple { second.edge, second.statement };
    });
    for (const Logged& entry : logged)
        m_statementOfEdge.add(entry.edge, entry.statement);
}

ValueFlowStatementID ValueFlowSolver::statementThatLogged(ValueFlowEdge edge) const
{
    return m_statementOfEdge.get(numberOf(edge));
}

const ValueFlowNodeKey* ValueFlowSolver::keyOfMadeNode(ValueFlowNodeID node) const
{
    return node.index() < m_input.numberOfNodes ? nullptr : &m_madeNodes[m_madeNodesOfNode[node.index() - m_input.numberOfNodes]].key;
}

Vector<ValueFlowNodeID> ValueFlowSolver::explain(ValueFlowNodeID start, ValueFlowObjectID member) const
{
    if (!m_sets.contains(valueOf(start), member))
        return { };

    // Breadth first, against the edges, through the nodes that hold the member.
    HashMap<uint32_t, ValueFlowNodeID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> cameFrom;
    Vector<ValueFlowNodeID> found { start };
    cameFrom.add(start.index(), ValueFlowNodeID());
    ValueFlowNodeID end;
    for (size_t next = 0; next < found.size() && !end; ++next) {
        ValueFlowNodeID node = found[next];
        const ValueFlowNodeKey* key = keyOfMadeNode(node);
        if (key && key->kind == ValueFlowMadeNodeKind::Constant) {
            end = node;
            break;
        }
        Vector<ValueFlowNodeID, 8> predecessors;
        m_graph.forEachPredecessor(node, [&](ValueFlowNodeID predecessor) {
            predecessors.append(predecessor);
        });
        std::ranges::sort(predecessors);
        for (ValueFlowNodeID predecessor : predecessors) {
            if (m_sets.contains(valueOf(predecessor), member) && cameFrom.add(predecessor.index(), node).isNewEntry)
                found.append(predecessor);
        }
    }

    Vector<ValueFlowNodeID> way;
    for (ValueFlowNodeID node = end; node; node = cameFrom.get(node.index()))
        way.append(node);
    way.reverse();
    return way;
}

bool ValueFlowSolver::merge()
{
    uint32_t numberOfNodesBefore = m_numberOfNodesBeforeRound;
    bool hasReached = !m_bodiesReachedInRound.isEmpty();

    countRunsAndEdges();
    if (m_input.remembersWhoLoggedEachEdge)
        rememberWhoLoggedTheEdges();
    for (const std::unique_ptr<Thread>& thread : m_threads) {
        m_counters.edgesLogged += thread->log.edges.size();
        for (ValueFlowEdge edge : thread->log.edges)
            m_edgesOfRound.append(ValueFlowEdge { thread->numberInGraph(edge.from), thread->numberInGraph(edge.to) });
    }

    std::ranges::sort(m_bodiesReachedInRound);
    for (uint32_t body : m_bodiesReachedInRound) {
        const ValueFlowBodyRange& range = m_input.ranges[body];
        const ValueFlowBodyRange& end = m_input.ranges[body + 1];
        m_edgesOfRound.append(m_input.edgesOnceReached.subspan(range.edgeOnceReached, end.edgeOnceReached - range.edgeOnceReached));
        for (const ValueFlowNodeSet& seed : m_input.seedsOnceReached.subspan(range.seedOnceReached, end.seedOnceReached - range.seedOnceReached)) {
            if (seed.set != ValueSets::nothing)
                m_edgesOfRound.append(ValueFlowEdge { ensureConstant(seed.set), seed.node });
        }
    }
    m_bodiesReachedInRound.shrink(0);

    linkNamedNodes();
    size_t numberOfEdgesAdded = m_graph.addEdges(m_edgesOfRound.span());
    m_edgesOfRound.shrink(0);

    bool hasUses = false;
    for (const std::unique_ptr<Thread>& thread : m_threads) {
        for (const ValueFlowRoundLog::Use& use : thread->log.uses) {
            if (use.builtin.index() >= m_usesOfBuiltin.size())
                m_usesOfBuiltin.grow(use.builtin.index() + 1);
            OptionSet<BuiltinUse>& uses = m_usesOfBuiltin[use.builtin.index()];
            hasUses |= !uses.containsAll(use.uses);
            uses.add(use.uses);
        }
    }

    noteInputs();
    for (const std::unique_ptr<Thread>& thread : m_threads) {
        thread->log.clear();
        thread->numberOfMadeNode.shrink(0);
        thread->numberOfReachedTaken = 0;
        thread->numberOfMadeTaken = 0;
        thread->numberOfRunsTaken = 0;
        thread->numberOfNodesAskedForTaken = 0;
    }
    return hasReached || hasUses || numberOfEdgesAdded || m_graph.numberOfNodes() != numberOfNodesBefore;
}

ValueSetID ValueFlowSolver::valueForReadersOf(ValueFlowNodeID node)
{
    ValueSetID value = valueOf(node);
    if (!m_input.unknownObject || node == m_input.escaped || !m_sets.contains(value, m_input.unknownObject))
        return value;
    const ValueFlowNodeKey* key = keyOfMadeNode(node);
    if (key && key->kind == ValueFlowMadeNodeKind::Part)
        return value;
    return m_makers[0]->unite(m_makers[0]->intersect(value, ValueSets::complement(valueOf(m_input.escaped))), m_unknownObjectAlone);
}

// Beside the unknown object a rule does not get to see what has escaped, so the arrival of such an object is no reason to run it again.
void ValueFlowSolver::makeReadersDue(std::span<const ValueFlowNodeID> moved)
{
    for (ValueFlowNodeID node : moved) {
        if (node.index() >= m_firstReaderOfNode.size() || m_firstReaderOfNode[node.index()] == none)
            continue;
        ValueSetID value = valueForReadersOf(node);
        if (m_sets.isSubset(value, m_seenByReadersOfNode[node.index()]))
            continue;
        m_seenByReadersOfNode[node.index()] = value;
        for (uint32_t reader = m_firstReaderOfNode[node.index()]; reader != none; reader = m_readers[reader].next)
            makeDue(m_readers[reader].statement);
    }
}

void ValueFlowSolver::spreadMarks()
{
    m_marksOfObject.fill(OptionSet<ObjectMutation>(), m_sets.sizeOfUniverse());
    for (ObjectMutation mutation : OptionSet<ObjectMutation>::all()) {
        std::optional<ValueFlowNodeID> node = findMadeNode(ValueFlowNodeKey::marked(mutation));
        if (!node)
            continue;
        m_sets.forEachMember(valueOf(*node), [&](ValueFlowObjectID object) {
            m_marksOfObject[object.index()].add(mutation);
        });
    }
}

bool ValueFlowSolver::solve()
{
    m_graph.addNodes(m_input.numberOfNodes);
    for (const ValueFlowNodeSet& mask : m_input.masks)
        m_graph.setMask(mask.node, mask.set);
    m_graph.addInitialEdges(m_input.edges);

    uint32_t numberOfBodies = m_input.ranges.size() - 1;
    for (uint32_t body = 0; body < numberOfBodies; ++body)
        makeStatementsDue(m_input.ranges[body].statement, m_input.ranges[body].statementLeavingBody);
    reach(numberOfBodies - 1, { });
    if (m_input.escaped) {
        ValueFlowStatement statement;
        statement.kind = ValueFlowStatementKind::Escape;
        m_madeStatements.append(MadeStatement { statement, { } });
        m_madeNodesOfStatement.append(none);
        m_isDue.ensureSize(numberOfStatements());
        makeDue(ValueFlowStatementID(numberOfStatements() - 1));
    }

    Vector<ValueFlowNodeID> moved;
    bool isFullRound = m_rounds == Rounds::Full;
    for (;;) {
        ++m_counters.rounds;
        m_counters.fullRounds += isFullRound;
        m_isFullRound = isFullRound;
        Counters before = m_counters;
        ValueFlowGraph::Counters graphBefore = m_graph.counters();
        size_t numberOfEdgesBefore = m_graph.numberOfEdges();

        MonotonicTime start = MonotonicTime::now();
        m_numberOfNodesBeforeRound = m_graph.numberOfNodes();
        if (!runRules())
            return false;
        MonotonicTime afterRules = MonotonicTime::now();
        bool hasAdded = merge();
        MonotonicTime afterMerge = MonotonicTime::now();
        m_graph.solve(moved);
        makeReadersDue(moved.span());
        m_counters.rules += afterRules - start;
        m_counters.merge += afterMerge - afterRules;
        m_counters.values += MonotonicTime::now() - afterMerge;

        m_roundsDone.append(Round {
            isFullRound,
            m_turnsOfRound,
            static_cast<uint32_t>(m_counters.statementRuns - before.statementRuns),
            m_counters.functionsReached - before.functionsReached,
            m_graph.numberOfNodes() - m_numberOfNodesBeforeRound,
            static_cast<uint32_t>(m_graph.numberOfEdges() - numberOfEdgesBefore),
            static_cast<uint32_t>(moved.size()),
            static_cast<uint32_t>(m_graph.counters().sweeps - graphBefore.sweeps),
            static_cast<uint32_t>(m_graph.counters().evaluations - graphBefore.evaluations),
            m_graph.counters().orderings != graphBefore.orderings,
        });

        bool isQuiet = !hasAdded && moved.isEmpty() && m_due.isEmpty();
        if (isQuiet && isFullRound)
            break;
        isFullRound = isQuiet || m_rounds == Rounds::Full;
        if (isFullRound)
            makeEveryStatementDue();
    }
    spreadMarks();
    return true;
}

OptionSet<BuiltinUse> ValueFlowSolver::usesOf(BuiltinID builtin) const
{
    return builtin.index() < m_usesOfBuiltin.size() ? m_usesOfBuiltin[builtin.index()] : OptionSet<BuiltinUse>();
}

OptionSet<ObjectMutation> ValueFlowSolver::marksOf(ValueFlowObjectID object) const
{
    return m_marksOfObject[object.index()];
}

size_t ValueFlowSolver::memoryInBytes() const
{
    auto bytesOf = [](const auto& vector) {
        return vector.capacity() * sizeof(vector[0]);
    };
    size_t bytes = m_graph.memoryInBytes() + m_sets.memoryInBytes() + bytesOf(m_madeNodes) + bytesOf(m_madeStatements) + bytesOf(m_firstReaderOfNode) + bytesOf(m_readers) + m_knownReaders.memoryInBytes() + bytesOf(m_nodesOfMembers);
    for (const Name& name : m_names) {
        for (const NodesOfName* nodes : { &name.read, &name.written })
            bytes += bytesOf(nodes->ofFewObjects) + bytesOf(nodes->ofManyObjects);
    }
    return bytes + bytesOf(m_names) + m_firstNodeOfMember.capacity() * 12;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
