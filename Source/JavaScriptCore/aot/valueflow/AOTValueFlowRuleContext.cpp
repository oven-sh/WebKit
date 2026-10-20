/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowRuleContext.h"

#if ENABLE(AOT)

#include "AOTValueFlowBodies.h"
#include "AOTValueFlowLog.h"
#include "AOTValueFlowSolver.h"

namespace JSC { namespace AOT { namespace Typed {

static ValueSetID constantOf(const ValueFlowOperand& operand)
{
    return operand.constant ? operand.constant : ValueSets::nothing;
}

static ValueSetID maskOf(const ValueFlowOperand& operand)
{
    return operand.mask ? operand.mask : ValueSets::anything;
}

ValueSetID SettledValues::valueOf(ValueFlowNodeID node) const
{
    return m_solved ? m_solved->valueOf(node) : ValueSets::anything;
}

ValueSetID SettledValues::valueOf(const ValueFlowOperand& operand, ValueSetMaker& maker) const
{
    ValueSetID value = constantOf(operand);
    if (operand.node)
        value = maker.unite(value, valueOf(operand.node));
    return maker.intersect(value, maskOf(operand));
}

ValueSetID SettledValues::read(ValueSetID, ValueFlowName, ValueSetMaker&) const
{
    RELEASE_ASSERT(!m_solved);
    return ValueSets::anything;
}

bool SettledValues::isReached(FunctionID function) const
{
    return !m_solved || m_solved->isReached(function);
}

OptionSet<ObjectMutation> SettledValues::marksOf(ValueFlowObjectID object) const
{
    return m_solved ? m_solved->marksOf(object) : OptionSet<ObjectMutation>::all();
}

OptionSet<ObjectMutation> SettledValues::marksOf(ValueSetID objects) const
{
    if (!m_solved)
        return OptionSet<ObjectMutation>::all();
    OptionSet<ObjectMutation> marks;
    m_solved->sets().forEachMember(objects, [&](ValueFlowObjectID object) {
        marks.add(m_solved->marksOf(object));
    });
    return marks;
}

OptionSet<BuiltinUse> SettledValues::usesOf(BuiltinID builtin) const
{
    return m_solved ? m_solved->usesOf(builtin) : OptionSet<BuiltinUse>::all();
}

ValueFlowRuleContext::ValueFlowRuleContext(const ValueFlowSolver& solver, ValueFlowRoundLog& log, ValueSetMaker& maker)
    : m_solver(solver)
    , m_log(log)
    , m_maker(maker)
{
    if (solver.m_input.unknownObject)
        m_unknownObjectAlone = maker.of(solver.m_input.unknownObject);
}

const Program& ValueFlowRuleContext::program() const
{
    return *m_solver.m_input.program;
}

const ValueFlowUniverse& ValueFlowRuleContext::universe() const
{
    return *m_solver.m_input.universe;
}

const ValueFlowProgramNodes& ValueFlowRuleContext::nodes() const
{
    return *m_solver.m_input.nodes;
}

void ValueFlowRuleContext::begin(ValueFlowStatementID statement)
{
    m_statement = statement;
}

void ValueFlowRuleContext::end()
{
    m_log.runs.append(ValueFlowRoundLog::Run { m_statement, static_cast<uint32_t>(m_log.nodesRead.size()), static_cast<uint32_t>(m_log.functionsAsked.size()), static_cast<uint32_t>(m_log.nodesAskedFor.size()), static_cast<uint32_t>(m_log.edges.size()), static_cast<uint32_t>(m_log.reached.size()), std::exchange(m_log.numberOfConnectionsOfRun, 0u), static_cast<uint32_t>(m_log.made.size()) });
}

FunctionID ValueFlowRuleContext::functionOfStatement() const
{
    return m_solver.statementAt(m_statement).function;
}

std::span<const ValueFlowOperand> ValueFlowRuleContext::operandsOf(const ValueFlowStatement& statement) const
{
    return m_solver.operandsOf(statement);
}

ClimbingSet ValueFlowRuleContext::setOfKey() const
{
    return ClimbingSet(m_solver.keyOf(m_statement).set);
}

ValueFlowName ValueFlowRuleContext::nameOfKey() const
{
    return m_solver.keyOf(m_statement).name();
}

ValueFlowCallKind ValueFlowRuleContext::callKindOfKey() const
{
    return m_solver.keyOf(m_statement).callKind();
}

ValueFlowBehaviorID ValueFlowRuleContext::behaviorOfKey() const
{
    return m_solver.keyOf(m_statement).behavior();
}

ClimbingSet ValueFlowRuleContext::valueOf(ValueFlowNodeID node)
{
    m_log.nodesRead.append(node);
    ClimbingValues values(m_solver.graph().values());
    ClimbingSet value = values.at(node);
    if (!m_unknownObjectAlone || node == m_solver.m_input.escaped || !contains(value, m_solver.m_input.unknownObject))
        return value;
    // What is left out here has been handed on by what earlier rounds have logged, and the rule for what escapes covers it from now on.
    return unite(restrict(value, ValueSets::complement(values.at(m_solver.m_input.escaped).m_set)), ClimbingSet(m_unknownObjectAlone));
}

ClimbingSet ValueFlowRuleContext::valueOf(const ValueFlowOperand& operand)
{
    ValueSetID value = constantOf(operand);
    if (operand.node)
        value = m_maker.unite(value, valueOf(operand.node).m_set);
    return ClimbingSet(m_maker.intersect(value, maskOf(operand)));
}

ClimbingSet ValueFlowRuleContext::valueWithin(ClimbingSet within, ValueFlowNodeID node)
{
    // The node that is read starts with what is answered here, so only what comes later moves it.
    m_log.nodesRead.append(nodeFor(ValueFlowNodeKey::part(node, within.m_set)));
    return restrict(ClimbingValues(m_solver.graph().values()).at(node), within.m_set);
}

ClimbingSet ValueFlowRuleContext::markedAmong(ClimbingSet objects, OptionSet<ObjectMutation> mutations)
{
    ClimbingSet marked;
    for (ObjectMutation mutation : mutations)
        marked = unite(marked, valueWithin(objects, nodeFor(ValueFlowNodeKey::marked(mutation))));
    return marked;
}

bool ValueFlowRuleContext::isReached(FunctionID function)
{
    m_log.functionsAsked.append(function);
    return m_solver.isReached(function);
}

const SettledValues& ValueFlowRuleContext::settled() const
{
    return m_solver.settled();
}

ValueFlowNodeID ValueFlowRuleContext::nodeFor(const ValueFlowNodeKey& key)
{
    bool namesNodeOfThisRound = (key.kind == ValueFlowMadeNodeKind::Masked || key.kind == ValueFlowMadeNodeKind::Part) && ValueFlowRoundLog::isNumberedByLog(key.maskedNode());
    std::optional<ValueFlowNodeID> existing = namesNodeOfThisRound ? std::nullopt : m_solver.findMadeNode(key);
    ValueFlowNodeID node = existing ? *existing : m_log.nodeFor(key);
    if (key.kind != ValueFlowMadeNodeKind::Constant && key.kind != ValueFlowMadeNodeKind::Marked)
        m_log.nodesAskedFor.append(node);
    return node;
}

ValueFlowNodeID ValueFlowRuleContext::read(ClimbingSet objects, ValueFlowName name)
{
    if (objects.m_set == ValueSets::nothing)
        return nodeFor(ValueFlowNodeKey::constant(ValueSets::nothing));
    return nodeFor(ValueFlowNodeKey::named(ValueFlowMadeNodeKind::Read, objects.m_set, name));
}

ValueFlowNodeID ValueFlowRuleContext::written(ClimbingSet objects, ValueFlowName name)
{
    return nodeFor(ValueFlowNodeKey::named(ValueFlowMadeNodeKind::Written, objects.m_set, name));
}

ValueFlowNodeID ValueFlowRuleContext::lookup(ClimbingSet receivers, ValueFlowName name)
{
    if (receivers.m_set == ValueSets::nothing)
        return nodeFor(ValueFlowNodeKey::constant(ValueSets::nothing));
    return nodeFor(ValueFlowNodeKey::named(ValueFlowMadeNodeKind::Lookup, receivers.m_set, name));
}

ValueFlowCallGroup ValueFlowRuleContext::group(ClimbingSet callees, ValueFlowCallKind kind)
{
    return callGroupAt(nodeFor(ValueFlowNodeKey::callGroup(callees.m_set, kind)));
}

ValueFlowNodeID ValueFlowRuleContext::hub(ValueFlowBehaviorID behavior)
{
    // The hubs of a program are nodes of the host's body, which also holds their statements.
    if (m_solver.m_input.nodes)
        return nodes().hub(behavior);
    return nodeFor(ValueFlowNodeKey::hub(behavior));
}

ValueFlowNodeID ValueFlowRuleContext::masked(ValueFlowNodeID node, ValueSetID mask)
{
    if (mask == ValueSets::anything)
        return node;
    return nodeFor(ValueFlowNodeKey::masked(node, mask));
}

ValueFlowNodeID ValueFlowRuleContext::holding(ClimbingSet set)
{
    return nodeFor(ValueFlowNodeKey::constant(set.m_set));
}

void ValueFlowRuleContext::connect(ValueFlowNodeID from, ValueFlowNodeID to)
{
    // A statement whose result nobody reads has no node for it.
    if (!from || !to)
        return;
    ++m_log.numberOfConnectionsOfRun;
    // A statement that runs again asks for most of its edges again.
    if (m_solver.graph().hasAdded(ValueFlowEdge { from, to }))
        return;
    if (m_log.edges.size() >= m_log.mostEdges) [[unlikely]] {
        m_log.hasDroppedEdges = true;
        return;
    }
    m_log.edges.append(ValueFlowEdge { from, to });
}

void ValueFlowRuleContext::connect(const ValueFlowOperand& operand, ValueFlowNodeID to)
{
    if (operand.node)
        connect(masked(operand.node, maskOf(operand)), to);
    add(to, m_maker.intersect(constantOf(operand), maskOf(operand)));
}

void ValueFlowRuleContext::add(ValueFlowNodeID node, ClimbingSet set)
{
    if (set.m_set != ValueSets::nothing)
        connect(nodeFor(ValueFlowNodeKey::constant(set.m_set)), node);
}

void ValueFlowRuleContext::reach(FunctionID function)
{
    if (!m_solver.isReached(function))
        m_log.reached.append(function);
}

void ValueFlowRuleContext::mark(ClimbingSet objects, OptionSet<ObjectMutation> mutations)
{
    if (objects.m_set == ValueSets::nothing)
        return;
    for (ObjectMutation mutation : mutations)
        add(nodeFor(ValueFlowNodeKey::marked(mutation)), objects);
}

void ValueFlowRuleContext::use(BuiltinID builtin, OptionSet<BuiltinUse> uses)
{
    m_log.uses.append(ValueFlowRoundLog::Use { builtin, uses });

    // The host's rules read the built-ins that are used as sets, like everything else that moves.
    if (!m_solver.m_input.nodes || !m_solver.m_input.universe)
        return;
    ClimbingSet object(m_maker.of(universe().objectOf(builtin)));
    if (!uses.containsOnly(BuiltinUse::Tested))
        add(nodes().ofHost(ValueFlowHostNode::UsedBuiltins), object);
    if (uses.contains(BuiltinUse::Written))
        add(nodes().ofHost(ValueFlowHostNode::WrittenBuiltins), object);
    if (uses.containsAny({ BuiltinUse::Called, BuiltinUse::Constructed, BuiltinUse::Escaped }))
        add(nodes().ofHost(ValueFlowHostNode::RunBuiltins), object);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
