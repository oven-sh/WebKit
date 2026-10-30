/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowRuleContext.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFlowResults.h"
#include "AOTThreshold.h"
#include "AOTValueFlowBodies.h"
#include "AOTValueFlowBuiltinTables.h"
#include "AOTValueFlowBuiltins.h"
#include "AOTValueFlowLog.h"
#include "AOTValueFlowSolver.h"

namespace JSC { namespace AOT { namespace Typed {

static ValueSetID constantOf(const ValueFlowOperand& operand) { return operand.constant ? operand.constant : ValueSets::nothing; }
static ValueSetID maskOf(const ValueFlowOperand& operand) { return operand.mask ? operand.mask : ValueSets::anything; }

static ValueSetID valueHeldInGraph(const ValueFlowGraph& graph, ValueFlowNodeID node)
{
    std::span<const ValueSetID> values = graph.values();
    return node.index() < values.size() ? values[node.index()] : ValueSets::nothing;
}

ValueFlowRuleContext::ValueFlowRuleContext(const ValueFlowSolver& solver, ValueFlowRoundLog& log, ValueSetMaker& maker)
    : m_solver(solver)
    , m_log(log)
    , m_instancedFunctions(solver.m_input.instancedFunctions)
    , m_skipsRedundantMarks(!solver.m_input.isAskedHowValuesCame && !solver.m_input.remembersWhoLoggedEachEdge)
    , m_maker(maker)
{
}

const Program& ValueFlowRuleContext::program() const { return *m_solver.m_input.program; }
const ValueFlowUniverse& ValueFlowRuleContext::universe() const { return *m_solver.m_input.universe; }
const ValueFlowProgramNodes& ValueFlowRuleContext::nodes() const { return *m_solver.m_input.nodes; }
uint64_t ValueFlowRuleContext::mostMembersToldApart() const { return m_solver.m_input.mostMembersToldApart; }

void ValueFlowRuleContext::begin(ValueFlowStatementID statement)
{
    m_statement = statement;
    m_madeNodesOfStatement = m_solver.isMadeByRule(statement) && statement.index() < m_solver.numberOfStatements() ? m_solver.m_madeNodesOfStatement[statement.index() - m_solver.m_input.statements.size()] : UINT32_MAX;
    ++m_runEpoch;
    m_isFirstRunOfStatementOfBody = m_solver.hasStatementOfBodyNeverRun(statement);
    m_filtersKnownEdges = !m_isFirstRunOfStatementOfBody || m_solver.m_isFullRound;
    m_firstEdgeOfRun = m_log.edges.size();
    m_firstNodeReadOfRun = m_log.nodesRead.size();
    m_firstNodeAskedWhetherEmptyOfRun = m_log.nodesAskedWhetherEmpty.size();
    m_firstNameWatchedOfRun = m_log.namesWatched.size();
    m_unknownSource = std::nullopt;
    m_holdingNothing = { };
    if (!m_constantsOfThisTurn.isEmpty())
        m_constantsOfThisTurn.clear();
    if (m_turnOfInstancesMade != m_solver.m_numberOfTurn) [[unlikely]] {
        m_turnOfInstancesMade = m_solver.m_numberOfTurn;
        m_instancesMadeInTurn.clear();
    }
}

void ValueFlowRuleContext::beginWhereMade(ValueFlowStatementID statement, const ValueFlowNodeKey& key, uint32_t madeNodes)
{
    begin(statement);
    m_madeNodesOfStatement = madeNodes;
    m_keyOfStatementOfThisTurn = &key;
    m_filtersKnownEdges = m_solver.m_isFullRound;
    m_rememberedByStatementOfThisTurn = { };
}

const ValueFlowNodeKey& ValueFlowRuleContext::keyOfStatement() const { return m_keyOfStatementOfThisTurn ? *m_keyOfStatementOfThisTurn : m_solver.keyOf(m_statement); }

void ValueFlowRuleContext::leaveOutWhatIsKnown()
{
    constexpr size_t mostAtOnce = 32;
    const ValueFlowGraph& graph = m_solver.graph();
    bool mayBeKnown = !hasNeverRun();
    size_t endOfEdges = m_log.edges.size();
    size_t endOfNodesRead = m_log.nodesRead.size();
    size_t endOfNodesAskedWhetherEmpty = m_log.nodesAskedWhetherEmpty.size();
    if (mayBeKnown) {
        size_t kept = m_firstNodeReadOfRun;
        for (size_t first = m_firstNodeReadOfRun; first < endOfNodesRead; first += mostAtOnce) {
            size_t end = std::min(endOfNodesRead, first + mostAtOnce);
            for (size_t index = first; index < end; ++index)
                m_solver.prefetchWhetherKnownReader(m_log.nodesRead[index], m_statement);
            if (first == m_firstNodeReadOfRun) {
                for (size_t index = m_firstNodeAskedWhetherEmptyOfRun; index < std::min(endOfNodesAskedWhetherEmpty, m_firstNodeAskedWhetherEmptyOfRun + mostAtOnce); ++index)
                    m_solver.prefetchWhetherKnownToWaitForSomethingIn(m_log.nodesAskedWhetherEmpty[index], m_statement);
            }
            for (size_t index = first; index < end; ++index) {
                ValueFlowNodeID node = m_log.nodesRead[index];
                if (!m_solver.isKnownReader(node, m_statement))
                    m_log.nodesRead[kept++] = node;
            }
        }
        m_log.nodesRead.shrink(kept);
        kept = m_firstNodeAskedWhetherEmptyOfRun;
        for (size_t index = m_firstNodeAskedWhetherEmptyOfRun; index < endOfNodesAskedWhetherEmpty; ++index) {
            ValueFlowNodeID node = m_log.nodesAskedWhetherEmpty[index];
            if (!m_solver.isKnownToWaitForSomethingIn(node, m_statement))
                m_log.nodesAskedWhetherEmpty[kept++] = node;
        }
        m_log.nodesAskedWhetherEmpty.shrink(kept);
        if (m_log.namesWatched.size() != m_firstNameWatchedOfRun) [[unlikely]] {
            kept = m_firstNameWatchedOfRun;
            for (size_t index = m_firstNameWatchedOfRun; index < m_log.namesWatched.size(); ++index) {
                ValueFlowRoundLog::NameWatched watched = m_log.namesWatched[index];
                if (m_solver.partOfNameWatchedBy(m_statement, watched.numberOfName) != watched.within)
                    m_log.namesWatched[kept++] = watched;
            }
            m_log.namesWatched.shrink(kept);
        }
    }
    if (!m_filtersKnownEdges) {
        if (endOfEdges > m_log.mostEdges) [[unlikely]] {
            m_log.hasDroppedEdges = true;
            m_log.edges.shrink(std::max<size_t>(m_firstEdgeOfRun, m_log.mostEdges));
        }
        return;
    }
    size_t kept = m_firstEdgeOfRun;
    std::span<ValueFlowEdge> edges = m_log.edges.mutableSpan();
    for (size_t first = m_firstEdgeOfRun; first < endOfEdges; first += mostAtOnce) {
        std::span<ValueFlowEdge> batch = edges.subspan(first, std::min(endOfEdges - first, mostAtOnce));
        static_assert(mostAtOnce <= 32);
        uint32_t present = 0;
        uint32_t undecided = 0;
        for (size_t index = 0; index < batch.size(); ++index) {
            ValueFlowGraph::EdgeLookup lookup = graph.lookUpInInlineLinks(batch[index]);
            present |= static_cast<uint32_t>(lookup == ValueFlowGraph::EdgeLookup::Present) << index;
            undecided |= static_cast<uint32_t>(lookup == ValueFlowGraph::EdgeLookup::NeedsHasAdded) << index;
        }
        for (; undecided; undecided &= undecided - 1) {
            unsigned index = std::countr_zero(undecided);
            present |= static_cast<uint32_t>(graph.hasAdded(batch[index])) << index;
        }
        for (size_t index = 0; index < batch.size(); ++index) {
            if (present >> index & 1)
                continue;
            if (kept >= m_log.mostEdges) [[unlikely]] {
                m_log.hasDroppedEdges = true;
                break;
            }
            edges[kept++] = batch[index];
        }
    }
    m_log.edges.shrink(kept);
}

void ValueFlowRuleContext::end()
{
    m_narrowedOperandsOfRun.shrink(0);
    leaveOutWhatIsKnown();
    if (m_keyOfStatementOfThisTurn) {
        if (m_rememberedByStatementOfThisTurn)
            m_log.rememberedByStatementsRunWhereMade.append({ m_statement, m_rememberedByStatementOfThisTurn });
        m_keyOfStatementOfThisTurn = nullptr;
    }
    m_log.runs.append(ValueFlowRoundLog::Run {
        m_statement,
        static_cast<uint32_t>(m_log.nodesRead.size()),
        static_cast<uint32_t>(m_log.functionsAsked.size()),
        static_cast<uint32_t>(m_log.madeNodesAskedFor.size()),
        static_cast<uint32_t>(m_log.edges.size()),
        static_cast<uint32_t>(m_log.reached.size()),
        std::exchange(m_log.numberOfConnectionsOfRun, 0u),
        static_cast<uint32_t>(m_log.made.size() + m_log.numberMadeWhereAskedFor),
        static_cast<uint32_t>(m_log.valuesShown.size()),
        static_cast<uint32_t>(m_log.partsWatched.size()),
        static_cast<uint32_t>(m_log.nodesAskedWhetherEmpty.size()),
        static_cast<uint32_t>(m_log.namesWatched.size()),
    });
}

FunctionID ValueFlowRuleContext::functionOfStatement() const { return m_keyOfStatementOfThisTurn ? FunctionID() : m_solver.statementAt(m_statement).function; }
// `if (!$isProxyObject(x)) use(x);`: the rules see x as the test lets it through. What the results read stays as it is: a mask there is a claim about the value, not about one use of it.
std::span<const ValueFlowOperand> ValueFlowRuleContext::operandsOf(const ValueFlowStatement& statement)
{
    if (!m_operandsInPlace.empty())
        return m_operandsInPlace;
    std::span<const ValueFlowOperand> operands = m_solver.operandsOf(statement);
    if (!statement.guardsOfOperands) [[likely]]
        return operands;
    std::span<const ValueFlowOperand> guards = m_solver.guardsOf(statement);
    m_narrowedOperandsOfRun.append(makeUnique<Vector<ValueFlowOperand, 8>>(operands));
    Vector<ValueFlowOperand, 8>& narrowed = *m_narrowedOperandsOfRun.last();
    for (size_t index = 0; index < std::min<size_t>(narrowed.size(), 8); ++index) {
        for (size_t position = 0; position < std::min<size_t>(guards.size(), 4); ++position) {
            if (!(statement.guardsOfOperands >> (index * 4 + position) & 1) || !guards[position].mask)
                continue;
            ValueFlowOperand& operand = narrowed[index];
            // A mask of another kind lets through more than the members of its set: the operand is what it lets through, cut by the test.
            if (operand.maskKind != ValueFlowMaskKind::Plain) {
                operand = ValueFlowOperand { { }, m_maker.intersect(valueOf(operand).m_set, guards[position].mask), { } };
                continue;
            }
            operand.mask = operand.mask ? m_maker.intersect(operand.mask, guards[position].mask) : guards[position].mask;
        }
    }
    return narrowed.span();
}
ValueFlowNodeID ValueFlowRuleContext::markersOf(FunctionID function) { return nodeFor(ValueFlowNodeKey::markersOf(function)); }

bool ValueFlowRuleContext::canBeReached(const ValueFlowStatement& statement)
{
    for (const ValueFlowOperand& guard : m_solver.guardsOf(statement)) {
        if (!meet(valueOf(guard), ValueSets::anything))
            return false;
    }
    return true;
}
void ValueFlowRuleContext::makeInstanceOfOtherCalls(FunctionID function) { makeInstance(*instancedFunctions()->find(function), ValueFlowInstancedFunctions::markerOfOtherCalls()); }

ValueFlowNodeID ValueFlowRuleContext::restArgumentEntered(FunctionID function, unsigned index)
{
    if (!instancedFunctions() || !instancedFunctions()->find(function))
        return { };
    return nodeFor(ValueFlowNodeKey::restArgument(function, index, m_maker.of(m_markerEntered ? m_markerEntered : ValueFlowInstancedFunctions::markerOfOtherCalls())));
}

ValueFlowNodeID ValueFlowRuleContext::restArgumentOfRun(FunctionID function, unsigned index)
{
    if (!m_markerOfRun)
        return { };
    return nodeFor(ValueFlowNodeKey::restArgument(function, index, m_maker.of(m_markerOfRun)));
}

ValueFlowNodeID ValueFlowRuleContext::instanceOf(ValueFlowNodeID node, ValueFlowObjectID marker) { return nodeFor(ValueFlowNodeKey::instance(node, m_maker.of(marker))); }

ALWAYS_INLINE ValueFlowNodeID ValueFlowRuleContext::nodeRead(ValueFlowNodeID node)
{
    if (!node || node.index() < m_firstNodeOfInstance || node.index() >= m_endOfNodesOfInstance)
        return node;
    return instanceOf(node, m_markerOfRun);
}

ALWAYS_INLINE ValueFlowNodeID ValueFlowRuleContext::nodeHandedTo(ValueFlowNodeID node)
{
    if (!m_instancedFunctions || !m_instancedFunctions->holdsNode(node)) [[likely]]
        return node;
    return instanceHandedTo(node);
}

NEVER_INLINE ValueFlowNodeID ValueFlowRuleContext::instanceHandedTo(ValueFlowNodeID node)
{
    if (node.index() >= m_firstNodeOfInstance && node.index() < m_endOfNodesOfInstance)
        return instanceOf(node, m_markerOfRun);
    ValueFlowObjectID marker = m_markerEntered ? m_markerEntered : ValueFlowInstancedFunctions::markerOfOtherCalls();
    makeInstance(*m_instancedFunctions->find(nodes().functionOf(node)), marker);
    return instanceOf(node, marker);
}

void ValueFlowRuleContext::makeInstance(const ValueFlowInstancedFunction& instanced, ValueFlowObjectID marker)
{
    uint32_t firstNode = nodes().firstNodeOf(instanced.function);
    uint32_t endOfNodes = nodes().endOfNodesOf(instanced.function);
    ValueSetID markerAlone = m_maker.of(marker);
    uint64_t name = static_cast<uint64_t>(instanced.function.index()) << 32 | marker.index();
    if (m_instancesMadeInTurn.contains(name) || m_solver.findNodeMadeBeforeThisTurn(ValueFlowNodeKey::instance(ValueFlowNodeID(firstNode), markerAlone)))
        return;
    m_instancesMadeInTurn.add(name);

    connectAsNamed(nodeFor(ValueFlowNodeKey::constant(markerAlone)), markersOf(instanced.function));
    for (uint32_t index = firstNode; index < endOfNodes; ++index)
        connectAsNamed(instanceOf(ValueFlowNodeID(index), marker), ValueFlowNodeID(index));
    for (const ValueFlowEdge& edge : instanced.edges) {
        bool comesFromBody = edge.from.index() >= firstNode && edge.from.index() < endOfNodes;
        connectAsNamed(comesFromBody ? instanceOf(edge.from, marker) : edge.from, instanceOf(edge.to, marker));
    }
    for (const ValueFlowNodeSet& seed : instanced.seeds)
        connectAsNamed(nodeFor(keyOfConstant(seed.set)), instanceOf(seed.node, marker));
}

ValueFlowRuleContext::InstanceOfRun::InstanceOfRun(ValueFlowRuleContext& context, FunctionID function, ValueFlowObjectID marker)
    : m_context(context)
{
    RELEASE_ASSERT(!context.m_markerOfRun);
    ++context.m_runEpoch;
    context.m_firstNodeOfInstance = context.nodes().firstNodeOf(function);
    context.m_endOfNodesOfInstance = context.nodes().endOfNodesOf(function);
    context.m_markerOfRun = marker;
}

ValueFlowRuleContext::InstanceOfRun::~InstanceOfRun()
{
    ++m_context.m_runEpoch;
    m_context.m_firstNodeOfInstance = 0;
    m_context.m_endOfNodesOfInstance = 0;
    m_context.m_markerOfRun = { };
}

ClimbingSet ValueFlowRuleContext::setOfKey() const { return ClimbingSet(keyOfStatement().set); }
uint32_t ValueFlowRuleContext::numberOfMembersOfKey() const { return sets().size(keyOfStatement().set); }
ValueFlowName ValueFlowRuleContext::nameOfKey() const { return keyOfStatement().name(); }
bool ValueFlowRuleContext::isLookupForCall() const { return !isStatementOfBody() && keyOfStatement().isLookupForCall(); }
ValueFlowCallKind ValueFlowRuleContext::callKindOfKey() const { return keyOfStatement().callKind(); }
ValueFlowObjectID ValueFlowRuleContext::markerOfKey() const { return keyOfStatement().markerOfCallGroup(); }

std::optional<ValueSetID> ValueFlowRuleContext::setOfConstantOfThisTurn(ValueFlowNodeID node) const
{
    if (m_constantsOfThisTurn.isEmpty()) [[likely]]
        return std::nullopt;
    return m_constantsOfThisTurn.getOptional(node.index());
}

ClimbingSet ValueFlowRuleContext::valueOf(ValueFlowNodeID named)
{
    ValueFlowNodeID node = nodeRead(named);
    if (std::optional<ValueSetID> constant = setOfConstantOfThisTurn(node))
        return ClimbingSet(m_solver.withoutWhatOtherMembersCover(*constant, m_maker));
    m_log.nodesRead.append(node);
    return ClimbingSet(m_solver.valueForReadersOf(node, m_maker));
}

// A proxy comes to every place through the list of the followed ones, so whoever finds it in a value finds it listed.
ClimbingSet ValueFlowRuleContext::proxiesAskedByTheirTrapsIn(const ValueFlowOperand& operand)
{
    if (!m_solver.m_input.nodes)
        return { };
    ValueSetID followed = valueHeldInGraph(m_solver.graph(), nodes().ofHost(ValueFlowHostNode::ProxiesWhoseHandlerIsFollowed));
    if (followed == ValueSets::nothing)
        return { };
    ClimbingSet proxies = restrict(valueOf(operand), followed);
    if (meet(proxies, ValueSets::anything) && !m_keyOfStatementOfThisTurn)
        m_log.statementsThatAskProxiesByTheirTraps.append(m_statement);
    return proxies;
}

ClimbingSet ValueFlowRuleContext::withoutProxiesThatWalksPass(ClimbingSet values)
{
    if (!m_solver.m_input.nodes)
        return values;
    ValueSetID passed = valueHeldInGraph(m_solver.graph(), nodes().ofHost(ValueFlowHostNode::ProxiesThatHandOnWhatComesFromBelow));
    return passed == ValueSets::nothing ? values : restrict(values, ValueSets::complement(passed));
}

ClimbingSet ValueFlowRuleContext::proxiesThatWalksPassAmong(ClimbingSet values)
{
    if (!m_solver.m_input.nodes)
        return { };
    return restrict(values, valueHeldInGraph(m_solver.graph(), nodes().ofHost(ValueFlowHostNode::ProxiesThatHandOnWhatComesFromBelow)));
}

void ValueFlowRuleContext::noteThatStatementAsksProxiesOfConstructorByTheirTraps()
{
    if (m_solver.m_input.nodes && !m_keyOfStatementOfThisTurn && valueHeldInGraph(m_solver.graph(), nodes().ofHost(ValueFlowHostNode::ProxiesWhoseHandlerIsFollowed)) != ValueSets::nothing)
        m_log.statementsThatAskProxiesByTheirTraps.append(m_statement);
}

ClimbingSet ValueFlowRuleContext::followedProxiesAmong(ClimbingSet values)
{
    if (!m_solver.m_input.nodes)
        return { };
    return restrict(values, valueHeldInGraph(m_solver.graph(), nodes().ofHost(ValueFlowHostNode::ProxiesWhoseHandlerIsFollowed)));
}

void ValueFlowRuleContext::openProxiesAmong(ClimbingSet values)
{
    if (!m_solver.m_input.nodes)
        return;
    ValueSetID followed = valueHeldInGraph(m_solver.graph(), nodes().ofHost(ValueFlowHostNode::ProxiesWhoseHandlerIsFollowed));
    if (followed == ValueSets::nothing || !meet(values, followed))
        return;
    add(nodes().ofHost(ValueFlowHostNode::ProxiesThatAreOpen), restrict(values, followed));
}

bool ValueFlowRuleContext::holdsAnything(ValueFlowNodeID named)
{
    if (!named)
        return false;
    ValueFlowNodeID node = nodeRead(named);
    if (std::optional<ValueSetID> constant = setOfConstantOfThisTurn(node))
        return *constant != ValueSets::nothing;
    if (m_solver.valueForReadersOf(node, m_maker) != ValueSets::nothing)
        return true;
    m_log.nodesAskedWhetherEmpty.append(node);
    return false;
}

ValueSetID ValueFlowRuleContext::throughMaskOf(const ValueFlowOperand& operand, ValueSetID value, const ValueFlowSolver& solver, ValueSetMaker& maker)
{
    ValueSetID mask = maskOf(operand);
    if (value == ValueSets::nothing || mask == ValueSets::anything)
        return value;
    if (operand.maskKind == ValueFlowMaskKind::Plain)
        return maker.intersect(value, mask);
    ValueFlowCoveredObjects covered;
    if (solver.m_input.escaped)
        covered.byUnknown = solver.graph().valueOf(solver.m_input.escaped);
    if (solver.m_input.lost)
        covered.byUntracked = solver.graph().valueOf(solver.m_input.lost);
    return maker.pass(value, mask, solver.familyObjectsOf(mask, maker), operand.maskKind, solver.m_input.placeholders, covered);
}

ValueSetID ValueFlowRuleContext::valueShownToRules(const ValueFlowOperand& operand, const ValueFlowSolver& solver, ValueSetMaker& maker)
{
    ValueSetID value = constantOf(operand);
    if (operand.node)
        value = maker.unite(value, solver.valueForReadersOf(operand.node, maker));
    return throughMaskOf(operand, value, solver, maker);
}

ClimbingSet ValueFlowRuleContext::valueOf(const ValueFlowOperand& operand)
{
    ValueSetID value = constantOf(operand);
    if (operand.node)
        value = m_maker.unite(value, valueOf(operand.node).m_set);
    ValueSetID mask = maskOf(operand);
    if (operand.maskKind != ValueFlowMaskKind::Plain && mask != ValueSets::anything && value != ValueSets::nothing) {
        if (ValueSetID familyObjects = m_solver.familyObjectsOf(mask, m_maker)) {
            const ValueFlowPlaceholders& placeholders = m_solver.m_input.placeholders;
            ClimbingSet taken(m_maker.intersect(familyObjects, mask));
            if (m_solver.m_input.escaped && operand.maskKind == ValueFlowMaskKind::Checked && placeholders.unknown && sets().contains(value, placeholders.unknown))
                valueWithin(taken, m_solver.m_input.escaped);
            if (m_solver.m_input.lost && placeholders.untracked && sets().contains(value, placeholders.untracked))
                valueWithin(taken, m_solver.m_input.lost);
        }
    }
    return ClimbingSet(throughMaskOf(operand, value, m_solver, m_maker));
}

ClimbingSet ValueFlowRuleContext::valueWithin(ClimbingSet within, ValueFlowNodeID named)
{
    if (within.m_set == ValueSets::nothing)
        return ClimbingSet();
    ValueFlowNodeID node = nodeRead(named);
    if (std::optional<ValueSetID> constant = setOfConstantOfThisTurn(node))
        return restrict(ClimbingSet(*constant), within.m_set);
    if (hasNeverRun() || !m_solver.isKnownWatcher(node, within.m_set, m_statement))
        m_log.partsWatched.append(ValueFlowNodeSet { node, within.m_set });
    if (ValueFlowRoundLog::isNumberedByLog(node))
        return ClimbingSet();
    return restrict(ClimbingSet(valueHeldInGraph(m_solver.graph(), node)), within.m_set);
}

ClimbingSet ValueFlowRuleContext::valueWithinAskedLast(ClimbingSet within, ValueFlowNodeID node)
{
    size_t numberWatched = m_log.partsWatched.size();
    ClimbingSet value = valueWithin(within, node);
    if (m_log.partsWatched.size() != numberWatched)
        m_log.partsWatchedLast.append(numberWatched);
    return value;
}

bool ValueFlowRuleContext::holdsAnyOf(ClimbingSet within, ValueFlowNodeID named)
{
    if (within.m_set == ValueSets::nothing)
        return false;
    uint64_t key = static_cast<uint64_t>(named.index()) << 32 | within.m_set.index();
    RunCacheEntry& cached = runCacheEntry(m_holdsAnyOfCache, key);
    if (cached.key == key && cached.epochAndValue >> 1 == m_runEpoch)
        return cached.epochAndValue & 1;
    cached.key = key;
    cached.epochAndValue = m_runEpoch << 1;
    ValueFlowNodeID node = nodeRead(named);
    if (std::optional<ValueSetID> constant = setOfConstantOfThisTurn(node)) {
        cached.epochAndValue |= m_maker.meet(*constant, within.m_set);
        return cached.epochAndValue & 1;
    }
    if (!ValueFlowRoundLog::isNumberedByLog(node) && m_maker.meet(valueHeldInGraph(m_solver.graph(), node), within.m_set)) {
        cached.epochAndValue |= 1;
        return true;
    }
    if (hasNeverRun() || !m_solver.isKnownWatcher(node, within.m_set, m_statement))
        m_log.partsWatched.append(ValueFlowNodeSet { node, within.m_set });
    return false;
}

ClimbingSet ValueFlowRuleContext::membersMissingNowFrom(ValueFlowNodeID node, ClimbingSet set) { return restrict(set, ValueSets::complement(m_maker.intersect(valueHeldInGraph(m_solver.graph(), node), set.m_set))); }
ClimbingSet ValueFlowRuleContext::membersMissingFrom(ValueFlowNodeID node, ClimbingSet set) { return restrict(set, ValueSets::complement(valueWithin(set, node).m_set)); }

ValueSetID& ValueFlowRuleContext::rememberedByStatement()
{
    if (m_keyOfStatementOfThisTurn)
        return m_rememberedByStatementOfThisTurn;
    if (m_solver.m_usesGeneralRulesOnly) {
        if (std::exchange(m_statementThatRemembersOnGeneralWay, m_statement) != m_statement)
            m_rememberedOnGeneralWay = { };
        return m_rememberedOnGeneralWay;
    }
    return m_solver.m_rememberedByStatement[m_statement.index()];
}

bool ValueFlowRuleContext::isStatementOfBody() const { return m_statement.index() < m_solver.m_input.statements.size(); }

bool ValueFlowRuleContext::wasAddedInEarlierRound(ValueFlowNodeID node, ClimbingSet set) const
{
    // Only the edges of earlier rounds have reached the graph's values, and all of them have.
    if ((!m_unknownSource || !leavesOutSomeUnknownSource()) && !sets().contains(set.m_set, m_solver.m_input.placeholders.unknown))
        return sets().isSubset(set.m_set, m_solver.graph().valueOf(node));
    std::optional<ValueFlowNodeID> holding = m_solver.findMadeNode(keyOfConstant(set.m_set));
    return holding && m_solver.graph().hasAdded(ValueFlowEdge { *holding, node });
}

bool ValueFlowRuleContext::isFirstToTieNamespaceInThisRound(ModuleID module)
{
    // Each edge must keep the statement that logs it first, whichever thread runs first.
    if (m_solver.m_input.remembersWhoLoggedEachEdge)
        return true;
    return m_solver.m_roundThatTiedNamespace[module.index()].exchange(m_solver.m_counters.rounds, std::memory_order_relaxed) != m_solver.m_counters.rounds;
}

ClimbingSet ValueFlowRuleContext::membersNewToThread(ClimbingSet set, unsigned number)
{
    ValueSetID before = m_rememberedByThread[number];
    if (sets().isSubset(set.m_set, before))
        return ClimbingSet();
    m_rememberedByThread[number] = m_maker.unite(before, set.m_set);
    return ClimbingSet(m_maker.intersect(set.m_set, ValueSets::complement(before)));
}

ClimbingSet ValueFlowRuleContext::newSince(ValueFlowNodeID named)
{
    ValueFlowNodeID node = nodeRead(named);
    ValueSetID held = valueOf(node).m_set;
    m_log.valuesShown.append(ValueFlowNodeSet { node, held });
    return ClimbingSet(m_maker.intersect(held, ValueSets::complement(m_solver.valueShownTo(m_statement, node))));
}

ClimbingSet ValueFlowRuleContext::markedAmong(ClimbingSet objects, OptionSet<ObjectMutation> mutations)
{
    ClimbingSet marked;
    for (ObjectMutation mutation : mutations)
        marked = unite(marked, valueWithin(objects, mutationNode(mutation)));
    return marked;
}

bool ValueFlowRuleContext::isReached(FunctionID function)
{
    if (m_solver.isReached(function))
        return true;
    m_log.functionsAsked.append(function);
    return false;
}

ALWAYS_INLINE void ValueFlowRuleContext::logMadeNodesAskedFor(const ValueFlowNodeKey& key, uint32_t madeNodes)
{
    if (key.kind == ValueFlowMadeNodeKind::Constant || key.kind == ValueFlowMadeNodeKind::Marked || key.kind == ValueFlowMadeNodeKind::Instance)
        return;
    if (madeNodes == m_madeNodesOfStatement)
        return;
    if (key.set == ValueSets::nothing && (key.kind == ValueFlowMadeNodeKind::Written || key.kind == ValueFlowMadeNodeKind::CallGroup))
        return;
    m_log.madeNodesAskedFor.append(madeNodes);
    if ((key.kind == ValueFlowMadeNodeKind::Lookup || key.kind == ValueFlowMadeNodeKind::CallGroup) && m_solver.isRecordAtRest(madeNodes)) [[unlikely]]
        m_log.askedForAtRest.append(madeNodes);
}

ValueFlowNodeID ValueFlowRuleContext::nodeFor(const ValueFlowNodeKey& key)
{
    uint32_t hash = key.hash();
    KnownNode& known = m_knownNodes[hash % m_knownNodes.size()];
    if (known.payloadAndSet != (static_cast<uint64_t>(key.payload) << 32 | key.set.index()) || known.kindAndDetail != (static_cast<uint32_t>(key.kind) << 8 | key.detail) || !known.node) [[unlikely]]
        return nodeForUncached(key, known, hash);
    logMadeNodesAskedFor(key, known.madeNodes);
    return known.node;
}

NEVER_INLINE ValueFlowNodeID ValueFlowRuleContext::nodeForUncached(const ValueFlowNodeKey& key, KnownNode& known, uint32_t hash)
{
    bool namesNodeOfThisRound = key.kind == ValueFlowMadeNodeKind::Masked && ValueFlowRoundLog::isNumberedByLog(key.maskedNode());
    std::optional<uint32_t> existing = namesNodeOfThisRound ? std::nullopt : m_solver.findOrMakeNodesWhereAskedFor(key, hash, m_log, m_maker);
    ValueFlowNodeID node = existing ? m_solver.m_madeNodes[*existing].first : m_log.nodeFor(key);
    if (existing && key.kind == ValueFlowMadeNodeKind::Constant && m_solver.m_madeNodes[*existing].isOfThisTurn)
        m_constantsOfThisTurn.add(node.index(), key.set);
    else if (existing)
        known = { static_cast<uint64_t>(key.payload) << 32 | key.set.index(), static_cast<uint32_t>(key.kind) << 8 | key.detail, node, *existing };
    logMadeNodesAskedFor(key, existing ? *existing : node.index());
    return node;
}

ValueFlowNodeID ValueFlowRuleContext::constantNodeForEdge(const ValueFlowNodeKey& key)
{
    uint32_t hash = key.hash();
    KnownNode& known = m_knownNodes[hash % m_knownNodes.size()];
    uint64_t detailAndSet = static_cast<uint64_t>(key.detail) << 32 | key.set.index();
    if (known.payloadAndSet == key.set.index() && known.kindAndDetail == key.detail && known.node) [[likely]]
        return known.node;
    uint32_t turn = static_cast<uint32_t>(m_solver.m_numberOfTurn);
    ConstantOfTurn& ofTurn = m_constantsOfTurn[hash / m_knownNodes.size() % m_constantsOfTurn.size()];
    if (ofTurn.detailAndSet == detailAndSet && ofTurn.turn == turn && ofTurn.node)
        return ofTurn.node;
    uint32_t number = *m_solver.findOrMakeNodesWhereAskedFor(key, hash, m_log, m_maker);
    ValueFlowNodeID node = m_solver.m_madeNodes[number].first;
    if (m_solver.m_madeNodes[number].isOfThisTurn)
        ofTurn = { detailAndSet, turn, node };
    else
        known = { key.set.index(), key.detail, node, number };
    return node;
}

ValueFlowNodeKey ValueFlowRuleContext::keyOfConstant(ValueSetID set) const
{
    ValueFlowObjectID unknownObject = m_solver.m_input.placeholders.unknown;
    if (m_unknownSource && unknownObject && (sets().contains(set, unknownObject) || leavesOut(*m_unknownSource)))
        return ValueFlowNodeKey::constant(set, *m_unknownSource);
    return ValueFlowNodeKey::constant(set);
}

bool ValueFlowRuleContext::leavesOut(ValueFlowUnknownSource source) const { return m_solver.m_input.unknownSourcesLeftOut.contains(source); }
bool ValueFlowRuleContext::leavesOutSomeUnknownSource() const { return !m_solver.m_input.unknownSourcesLeftOut.isEmpty(); }
RefusedCode ValueFlowRuleContext::refusedCode() const { return m_solver.m_input.refusedCode; }
bool ValueFlowRuleContext::isSabotaged(Sabotage part) const { return m_solver.m_input.build && m_solver.m_input.build->isSabotaged(part); }

bool ValueFlowRuleContext::isGuardedByRemovedFeature(BuiltinID builtin) const
{
    const FeatureBits& removedFeatures = m_solver.m_input.removedFeatures;
    if (std::ranges::none_of(removedFeatures, [](uint64_t word) { return !!word; }))
        return false;
    return Typed::isGuardedByRemovedFeature(program().builtins(), builtin, removedFeatures);
}

ValueFlowNodeID ValueFlowRuleContext::holdingNothing() { return m_holdingNothing = nodeFor(ValueFlowNodeKey::constant(ValueSets::nothing)); }

// The objects of `within` that declare a field of the name or are written under it. The statement runs again when one more of them is written under it. What its earlier runs have asked about the name is forgotten.
// Whoever asks about a name in the first round in which it is asked about gets no answer and runs again: the solver keeps the holders under a name from then on.
std::optional<ClimbingSet> ValueFlowRuleContext::holdersUnder(ValueFlowName name, ClimbingSet within)
{
    uint32_t number = m_solver.numberOfName(name);
    std::span<ValueFlowRoundLog::NameWatched> watchedInRun = m_log.namesWatched.mutableSpan().subspan(m_firstNameWatchedOfRun);
    auto earlier = std::ranges::find(watchedInRun, name, &ValueFlowRoundLog::NameWatched::name);
    if (earlier != watchedInRun.end())
        earlier->within = m_maker.unite(earlier->within, within.m_set);
    else
        m_log.namesWatched.append({ name, within.m_set, number });
    ValueSetID holders = m_solver.holdersUnder(number);
    if (!holders)
        return std::nullopt;
    return restrict(within, holders);
}

// A set of receivers that grows keeps its record for a name as long as none of the new receivers holds anything under that name.
NEVER_INLINE ClimbingSet ValueFlowRuleContext::objectsThatMayHoldSomethingUnder(ValueFlowName name, ClimbingSet objects)
{
    if (!m_solver.m_input.nodes || !m_solver.m_input.universe)
        return objects;
    std::optional<ClimbingSet> holders = holdersUnder(name, objects);
    if (!holders || holders->m_set == objects.m_set)
        return objects;
    return m_solver.mayBeComputedKey(name) ? unite(*holders, valueWithin(objects, writtenUnderUnknownKey())) : *holders;
}

ValueFlowNodeID ValueFlowRuleContext::readWhatWasWritten(ClimbingSet objects, ValueFlowName name)
{
    AOT_THRESHOLD(uint32_t, mostObjectsReadWhole, 8);
    ASSERT(!m_solver.m_input.program || !namesElementByText(*m_solver.m_input.program, name));
    if (name.kind == ValueFlowNameKind::Property && !ValueSets::isComplement(objects.m_set) && sets().size(objects.m_set) > mostObjectsReadWhole)
        objects = objectsThatMayHoldSomethingUnder(name, objects);
    if (objects.m_set == ValueSets::nothing)
        return holdingNothing();
    return nodeFor(ValueFlowNodeKey::named(ValueFlowMadeNodeKind::Read, objects.m_set, name));
}

std::optional<ClimbingSet> ValueFlowRuleContext::peekAtWhatWasWritten(ClimbingSet objects, ValueFlowName name)
{
    ASSERT(name.kind != ValueFlowNameKind::Property);
    if (objects.m_set == ValueSets::nothing)
        return ClimbingSet();
    std::optional<ValueFlowNodeID> node = m_solver.findNodeMadeBeforeThisTurn(ValueFlowNodeKey::named(ValueFlowMadeNodeKind::Read, objects.m_set, name));
    if (!node)
        return std::nullopt;
    return ClimbingSet(m_solver.valueForReadersOf(*node, m_maker));
}

std::optional<ClimbingSet> ValueFlowRuleContext::sourcesOfCopiesSeenFrom(ClimbingSet objects)
{
    if (m_turnOfSourcesOfCopiesSeen != m_solver.m_numberOfTurn)
        return std::nullopt;
    auto seen = m_sourcesOfCopiesSeenFrom.find(objects.m_set.index());
    if (seen == m_sourcesOfCopiesSeenFrom.end())
        return std::nullopt;
    return ClimbingSet(seen->value);
}

void ValueFlowRuleContext::noteSourcesOfCopiesSeenFrom(ClimbingSet objects, ClimbingSet seen)
{
    if (m_turnOfSourcesOfCopiesSeen != m_solver.m_numberOfTurn) {
        m_turnOfSourcesOfCopiesSeen = m_solver.m_numberOfTurn;
        m_sourcesOfCopiesSeenFrom.clear();
    }
    m_sourcesOfCopiesSeenFrom.set(objects.m_set.index(), seen.m_set);
}

void ValueFlowRuleContext::addObjectsOfProgram(ValueFlowNodeID node, ClimbingSet objects) { add(node, restrict(objects, ValueSets::complement(universe().placeholdersAlone()))); }

ValueFlowNodeID ValueFlowRuleContext::notPlain()
{
    if (m_solver.m_objectsThatAreNotPlain) [[likely]]
        return m_solver.m_objectsThatAreNotPlain;
    return nodeFor(ValueFlowNodeKey::objectsThatAreNotPlain());
}

ValueFlowNodeID ValueFlowRuleContext::writtenUnderUnknownKey()
{
    if (m_solver.m_objectsWrittenUnderUnknownKey) [[likely]]
        return m_solver.m_objectsWrittenUnderUnknownKey;
    return nodeFor(ValueFlowNodeKey::objectsWrittenUnderUnknownKey());
}

ValueFlowNodeID ValueFlowRuleContext::rareWriteNode(ValueFlowRareWrite write)
{
    if (m_solver.m_firstOfObjectsWithRareWrite) [[likely]]
        return ValueFlowNodeID(m_solver.m_firstOfObjectsWithRareWrite.index() + static_cast<uint32_t>(write));
    return nodeFor(ValueFlowNodeKey::objectsWithRareWrite(write));
}

ValueFlowNodeID ValueFlowRuleContext::mutationNode(ObjectMutation mutation)
{
    ValueFlowNodeID& cached = m_mutationNodes[std::countr_zero(static_cast<uint32_t>(mutation))];
    if (!cached) [[unlikely]] {
        ValueFlowNodeID node = nodeFor(ValueFlowNodeKey::marked(mutation));
        if (ValueFlowRoundLog::isNumberedByLog(node))
            return node;
        cached = node;
    }
    return cached;
}

bool ValueFlowRuleContext::mayFindSomethingWritten(ClimbingSet objects, ValueFlowNameKind kind)
{
    std::optional<ValueFlowRareWrite> write = rareWriteFoundUnder(kind);
    if (!write || !m_solver.m_input.nodes || !m_solver.m_input.universe || ValueSets::isComplement(objects.m_set) || meet(objects, universe().placeholdersAlone()))
        return true;
    return holdsAnyOf(objects, rareWriteNode(*write));
}

ClimbingSet ValueFlowRuleContext::holdersOfLibraryExportsWrittenUnder(PropertyKeyID key)
{
    return valueWithin(ClimbingSet(universe().objectsOfLibraryFoundByNameAlone()), nodeFor(ValueFlowNodeKey::holdersOfLibraryExportsWrittenUnder(key)));
}

bool ValueFlowRuleContext::mayFindWhatIsWrittenUnderUnknownKey(ValueFlowName name) const { return m_solver.mayBeComputedKey(name); }

bool ValueFlowRuleContext::usesGeneralRulesOnly() const { return m_solver.m_usesGeneralRulesOnly; }

ValueFlowNodeID ValueFlowRuleContext::written(ClimbingSet objects, ValueFlowName name)
{
    ASSERT(!m_solver.m_input.program || !namesElementByText(*m_solver.m_input.program, name));
    if (std::optional<ValueFlowRareWrite> write = rareWriteFoundUnder(name.kind); write && m_solver.m_input.nodes && m_solver.m_input.universe)
        addObjectsOfProgram(rareWriteNode(*write), objects);
    if (m_solver.m_input.nodes && m_solver.m_input.universe) {
        ValueFlowNameShape shape = shapeOf(name.kind);
        if ((shape.isAccessor() && shape.reach != ValueFlowNameReach::EveryKey) || shape.sort == ValueFlowNameSort::Deletion || shape.sort == ValueFlowNameSort::Redefinition || shape.sort == ValueFlowNameSort::Copy)
            addObjectsOfProgram(notPlain(), objects);
        else if (name.kind == ValueFlowNameKind::UnknownKey)
            addObjectsOfProgram(writtenUnderUnknownKey(), objects);
        if (shape.reach == ValueFlowNameReach::OneKey && shape.isValueOrAccessor() && meet(objects, universe().objectsOfLibraryFoundByNameAlone()))
            add(nodeFor(ValueFlowNodeKey::holdersOfLibraryExportsWrittenUnder(PropertyKeyID(name.payload))), restrict(objects, universe().objectsOfLibraryFoundByNameAlone()));
    }
    ValueFlowNodeID written = nodeFor(ValueFlowNodeKey::named(ValueFlowMadeNodeKind::Written, objects.m_set, name));
    if ((name.kind == ValueFlowNameKind::Property || name.kind == ValueFlowNameKind::Getter) && m_solver.m_input.nodes) {
        for (BuiltinName called : program().builtins().namesOfHostMethodsCallingStartupLoads()) {
            if (program().keyOf(called).index() == name.payload)
                connect(written, nodes().ofHost(ValueFlowHostNode::StoredUnderNamesOfMethodsCallingStartupLoads));
        }
    }
    return written;
}

bool ValueFlowRuleContext::runsCodeThatRecordsDescribe() const
{
    if (!m_solver.m_input.universe)
        return false;
    FunctionID function = functionOfStatement();
    return function && universe().builtinTables().isCodeThatRecordsDescribe(function);
}

ValueFlowNodeID ValueFlowRuleContext::lookup(ClimbingSet receivers, ValueFlowName name, LookupUse use)
{
    ASSERT(!m_solver.m_input.program || !namesElementByText(*m_solver.m_input.program, name));
    if (sets().contains(receivers.m_set, ValueFlowUniverse::unknownObject()) && runsCodeThatRecordsDescribe()) {
        ValueFlowNodeID found = lookup(ClimbingSet(m_maker.intersect(receivers.m_set, ValueSets::complement(universe().unknownObjectAlone()))), name);
        ValueFlowNodeID answer = masked(found, ValueSets::complement(m_maker.of(calleeOfDescribedCall(universe()))));
        noteUnknownSource(ValueFlowUnknownSource::ContentsOfEscapedObject);
        add(answer, ClimbingSet(leavesOut(ValueFlowUnknownSource::ContentsOfEscapedObject) ? universe().anyPrimitive() : m_maker.unite(universe().anyPrimitive(), universe().unknownObjectAlone())));
        return answer;
    }
    if (receivers.m_set == ValueSets::nothing)
        return holdingNothing();
    return nodeFor(use == LookupUse::OnlyCalledOnReceiver ? ValueFlowNodeKey::lookupForCall(receivers.m_set, name) : ValueFlowNodeKey::named(ValueFlowMadeNodeKind::Lookup, receivers.m_set, name));
}

ValueFlowCallGroup ValueFlowRuleContext::group(ClimbingSet callees, ValueFlowCallKind kind, ValueFlowCalleeOrigin origin)
{
    bool mayBeWhateverHasEscaped = origin == ValueFlowCalleeOrigin::Value && kind != ValueFlowCallKind::Get && kind != ValueFlowCallKind::Set;
    if (mayBeWhateverHasEscaped && m_solver.m_input.nodes && m_solver.m_input.universe && sets().contains(callees.m_set, ValueFlowUniverse::unknownObject()))
        add(nodes().ofHost(ValueFlowHostNode::ValueNobodyKnowsIsCalled), ClimbingSet(leavesOutSomeUnknownSource() ? universe().undefinedValue() : universe().unknownObjectAlone()));
    return callGroupAt(nodeFor(ValueFlowNodeKey::callGroup(callees.m_set, kind, m_markerEntered)));
}

ValueFlowNodeID ValueFlowRuleContext::hub(ValueFlowBehaviorID behavior)
{
    if (m_solver.m_input.nodes)
        return nodes().hub(behavior);
    return nodeFor(ValueFlowNodeKey::hub(behavior));
}

ValueFlowNodeID ValueFlowRuleContext::masked(ValueFlowNodeID node, ValueSetID mask, ValueFlowMaskKind kind)
{
    if (!mask || mask == ValueSets::anything)
        return node;
    return nodeFor(ValueFlowNodeKey::masked(nodeRead(node), mask, kind));
}

ValueFlowNodeID ValueFlowRuleContext::holding(ClimbingSet set) { return nodeFor(keyOfConstant(set.m_set)); }

void ValueFlowRuleContext::connect(ValueFlowNodeID from, ValueFlowNodeID to)
{
    if (from && from == m_holdingNothing) {
        nodeHandedTo(to);
        return;
    }
    connectAsNamed(nodeRead(from), nodeHandedTo(to));
}

void ValueFlowRuleContext::connectAsNamed(ValueFlowNodeID from, ValueFlowNodeID to)
{
    if (!from || !to || from == to)
        return;
    ++m_log.numberOfConnectionsOfRun;
    uint64_t key = static_cast<uint64_t>(from.index()) << 32 | to.index();
    RunCacheEntry& cached = runCacheEntry(m_edgeCache, key);
    if (cached.key == key && cached.epochAndValue == m_runEpoch)
        return;
    cached = { key, m_runEpoch };
    if (m_filtersKnownEdges)
        m_solver.graph().prefetchLinksThatHold(ValueFlowEdge { from, to });
    m_log.edges.append(ValueFlowEdge { from, to });
    AOT_THRESHOLD(size_t, mostEdgesThatWait, 1 << 16);
    if (m_log.edges.size() - m_firstEdgeOfRun >= mostEdgesThatWait) [[unlikely]] {
        size_t firstNodeRead = std::exchange(m_firstNodeReadOfRun, m_log.nodesRead.size());
        size_t firstNodeAskedWhetherEmpty = std::exchange(m_firstNodeAskedWhetherEmptyOfRun, m_log.nodesAskedWhetherEmpty.size());
        size_t firstNameWatched = std::exchange(m_firstNameWatchedOfRun, m_log.namesWatched.size());
        leaveOutWhatIsKnown();
        m_firstNameWatchedOfRun = firstNameWatched;
        m_firstEdgeOfRun = m_log.edges.size();
        m_firstNodeReadOfRun = firstNodeRead;
        m_firstNodeAskedWhetherEmptyOfRun = firstNodeAskedWhetherEmpty;
    }
}

void ValueFlowRuleContext::connect(const ValueFlowOperand& operand, ValueFlowNodeID to)
{
    if (!to)
        return;
    if (operand.node)
        connect(masked(operand.node, maskOf(operand), operand.maskKind), to);
    ValueSetID constant = constantOf(operand);
    const ValueFlowPlaceholders& placeholders = m_solver.m_input.placeholders;
    bool holdsPlaceholder = (placeholders.unknown && sets().contains(constant, placeholders.unknown)) || (placeholders.untracked && sets().contains(constant, placeholders.untracked));
    if (holdsPlaceholder && operand.maskKind != ValueFlowMaskKind::Plain)
        connect(masked(nodeFor(keyOfConstant(constant)), maskOf(operand), operand.maskKind), to);
    else
        add(to, throughMaskOf(operand, constant, m_solver, m_maker));
}

ALWAYS_INLINE bool ValueFlowRuleContext::isRedundantAdd(ValueFlowNodeID node, ValueSetID set)
{
    if (node.index() >= m_solver.graph().numberOfNodes())
        return false;
    if (m_instancedFunctions && m_instancedFunctions->holdsNode(node))
        return false;
    ValueSetID held = m_solver.graph().valueOf(node);
    return held == set || (held != ValueSets::nothing && !m_maker.meet(set, ValueSets::complement(held)));
}

void ValueFlowRuleContext::add(ValueFlowNodeID node, ClimbingSet set)
{
    if (set.m_set == ValueSets::nothing)
        return;
    if (isRedundantAdd(node, set.m_set)) {
        ++m_log.numberOfConnectionsOfRun;
        return;
    }
    uint64_t key = static_cast<uint64_t>(node.index()) << 32 | set.m_set.index();
    uint64_t epoch = addEpoch();
    RunCacheEntry& cached = runCacheEntry(m_addCache, key);
    if (cached.key == key && cached.epochAndValue >> 1 == epoch) {
        m_log.numberOfConnectionsOfRun += cached.epochAndValue & 1;
        return;
    }
    ValueFlowNodeID constant = constantNodeForEdge(keyOfConstant(set.m_set));
    cached = { key, epoch << 1 | (node && node != constant) };
    connect(constant, node);
}

void ValueFlowRuleContext::addUnlessHeld(ValueFlowNodeID node, ValueFlowObjectID object)
{
    if (!sets().contains(m_solver.possibleValuesOf(node), object))
        add(node, ClimbingSet(m_maker.of(object)));
}

void ValueFlowRuleContext::reach(FunctionID function)
{
    if (m_solver.isReached(function))
        return;
    m_log.reached.append(function);
    m_log.whoReached.append({ m_statement, m_keyOfStatementOfThisTurn ? m_solver.contentOf(*m_keyOfStatementOfThisTurn) : m_solver.contentOf(m_statement) });
}

void ValueFlowRuleContext::mark(ClimbingSet objects, OptionSet<ObjectMutation> mutations)
{
    if (objects.m_set == ValueSets::nothing)
        return;
    ValueFlowNodeID holdingObjects;
    for (ObjectMutation mutation : mutations) {
        ValueFlowNodeID marked = mutationNode(mutation);
        if (m_skipsRedundantMarks && isRedundantAdd(marked, objects.m_set)) {
            ++m_log.numberOfConnectionsOfRun;
            continue;
        }
        if (!holdingObjects)
            holdingObjects = constantNodeForEdge(ValueFlowNodeKey::constant(objects.m_set));
        connect(holdingObjects, marked);
    }
    if (m_solver.m_input.nodes && m_solver.m_input.universe && mutations.containsAny({ ObjectMutation::DeleteField, ObjectMutation::RedefineProperty, ObjectMutation::ChangePrototype }))
        addObjectsOfProgram(notPlain(), objects);
}

void ValueFlowRuleContext::use(BuiltinID builtin, OptionSet<BuiltinUse> uses)
{
    useAsAnswerOfUnknownObject(builtin, uses);
    if (m_solver.m_input.nodes && m_solver.m_input.universe && uses.containsAny({ BuiltinUse::Read, BuiltinUse::Written, BuiltinUse::Called, BuiltinUse::Constructed }))
        addUnlessHeld(nodes().ofHost(ValueFlowHostNode::BuiltinsThatStatementsName), universe().objectOf(builtin));
}

void ValueFlowRuleContext::useAsAnswerOfUnknownObject(BuiltinID builtin, OptionSet<BuiltinUse> uses)
{
    if (m_solver.usesOf(builtin).containsAll(uses))
        return;
    m_log.use(builtin, uses);

    if (!m_solver.m_input.nodes || !m_solver.m_input.universe)
        return;
    ClimbingSet object(m_maker.of(universe().objectOf(builtin)));
    bool onlyReplaces = uses.contains(BuiltinUse::Written) && program().builtins().at(program().builtins().canonicalID(builtin)).flags.contains(BuiltinFlag::IsLazilyCreated);
    if (uses.containsAny({ BuiltinUse::Read, BuiltinUse::Called, BuiltinUse::Constructed, BuiltinUse::Escaped }) || (uses.contains(BuiltinUse::Written) && !onlyReplaces))
        add(nodes().ofHost(ValueFlowHostNode::UsedBuiltins), object);
    if (uses.contains(BuiltinUse::Written))
        add(nodes().ofHost(ValueFlowHostNode::WrittenBuiltins), object);
    if (uses.containsAny({ BuiltinUse::Called, BuiltinUse::Constructed }))
        add(nodes().ofHost(ValueFlowHostNode::RunBuiltins), object);
}

void ValueFlowRuleContext::noteMembersNotToldApart() { m_log.statementsWithMembersNotToldApart.append(m_statement); }

FunctionID ValueFlowRuleContext::libraryFunctionToEnter(RuntimeFunctionID id)
{
    if (const RuntimeFunctionEntry* entry = findRuntimeFunction(program(), id); entry && entry->function)
        return entry->function;
    if (id && !m_solver.lacksLibraryFunction(id))
        m_log.libraryFunctionsLacked.append(id);
    return { };
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
