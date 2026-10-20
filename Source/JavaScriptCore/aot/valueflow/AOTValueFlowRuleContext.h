/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTValueFlow.h"
#include "AOTValueSets.h"
#include <span>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT { namespace Typed {

class ValueFlowProgramNodes;
class ValueFlowRoundLog;
class ValueFlowSolver;
class ValueFlowUniverse;
struct ValueFlowNodeKey;

// A set as it stands in this round; a later round may find more in it. So it answers only what stays true when it grows: its members one by one, whether it holds a member, whether it shares one
// with another set. It cannot be asked whether it is empty, how many members it has, what it lacks or whether it equals another set, and it has no complement. Such a question goes to SettledValues.
// A rule has no `else` after a question to a climbing set and nothing after a loop over its members that rests on what the loop did not find.
class ClimbingSet {
public:
    ClimbingSet() = default;

    // A set that never moves may stand where a climbing one is asked for.
    explicit ClimbingSet(ValueSetID constant)
        : m_set(constant)
    {
    }

private:
    friend class ClimbingValues;
    friend class ValueFlowRuleContext;

    ValueSetID m_set { ValueSets::nothing };
};

// What every node holds while the rules of a round run. Nothing moves in it until all of them have run.
class ClimbingValues {
public:
    ClimbingValues() = default;
    explicit ClimbingValues(std::span<const ValueSetID> setOfNode)
        : m_setOfNode(setOfNode)
    {
    }

    // A node made in this round holds nothing yet.
    ClimbingSet at(ValueFlowNodeID node) const { return ClimbingSet(node.index() < m_setOfNode.size() ? m_setOfNode[node.index()] : ValueSets::nothing); }

private:
    std::span<const ValueSetID> m_setOfNode;
};

// A finished solution that holds at least what the one being computed will hold: before the first solve the top of everything (every enforced typed position holds its family, every other position anything,
// every mark is set, every function is reached, every member of a built-in is used in every way). What is absent here is absent, so these sets answer every question of ValueSets.
class SettledValues {
public:
    ValueSetID valueOf(ValueFlowNodeID) const;
    ValueSetID valueOf(const ValueFlowOperand&, ValueSetMaker&) const;
    // What a read under the name through any of the objects may give.
    ValueSetID read(ValueSetID objects, ValueFlowName, ValueSetMaker&) const;
    bool isReached(FunctionID) const;
    OptionSet<ObjectMutation> marksOf(ValueFlowObjectID) const;
    // The marks that some member has.
    OptionSet<ObjectMutation> marksOf(ValueSetID) const;
    OptionSet<BuiltinUse> usesOf(BuiltinID) const;

private:
    friend class ValueFlowSolver;

    // Null before the first solve.
    const ValueFlowSolver* m_solved { nullptr };
};

// All that a rule may touch. A rule is a pure function of the frozen values of the round and of the settled solution into the log of its thread. It only adds.
// Whatever it reads of the moving state it reads here, and that read is what makes its statement due again once the answer has moved.
class ValueFlowRuleContext {
    WTF_MAKE_NONCOPYABLE(ValueFlowRuleContext);
public:
    ValueFlowRuleContext(const ValueFlowSolver&, ValueFlowRoundLog&, ValueSetMaker&);

    const Program& program() const;
    const ValueFlowUniverse& universe() const;
    const ValueFlowProgramNodes& nodes() const;

    // The statement that runs.
    ValueFlowStatementID statement() const { return m_statement; }
    // None for a statement that a rule has made and for one of the host.
    FunctionID functionOfStatement() const;
    std::span<const ValueFlowOperand> operandsOf(const ValueFlowStatement&) const LIFETIME_BOUND;
    // Of a statement that a rule has made: what lookup(), group() or hub() was asked for.
    ClimbingSet setOfKey() const;
    ValueFlowName nameOfKey() const;
    ValueFlowCallKind callKindOfKey() const;
    ValueFlowBehaviorID behaviorOfKey() const;

    // The moving state. A set that holds the unknown object comes without the objects that have escaped so far, for which the unknown object stands; the node of the escaped objects itself lists them all.
    ClimbingSet valueOf(ValueFlowNodeID);
    ClimbingSet valueOf(const ValueFlowOperand&);
    bool isReached(FunctionID);
    // What the node holds of the members of `within`, without the normal form. The statement is due again only when that part has moved, however much else arrives at the node.
    ClimbingSet valueWithin(ClimbingSet within, ValueFlowNodeID);
    // The members that have one of the marks so far. The statement is due again only when another of the members gets one.
    ClimbingSet markedAmong(ClimbingSet objects, OptionSet<ObjectMutation>);

    void forEachMember(ClimbingSet set, NOESCAPE const Invocable<void(ValueFlowObjectID)> auto& functor) const { sets().forEachMember(set.m_set, functor); }
    bool contains(ClimbingSet set, ValueFlowObjectID member) const { return sets().contains(set.m_set, member); }
    bool meet(ClimbingSet first, ClimbingSet second) const { return sets().meet(first.m_set, second.m_set); }
    bool meet(ClimbingSet set, ValueSetID constant) const { return sets().meet(set.m_set, constant); }
    ClimbingSet unite(ClimbingSet first, ClimbingSet second) { return ClimbingSet(m_maker.unite(first.m_set, second.m_set)); }
    // The mask is a constant; it may be a complement.
    ClimbingSet restrict(ClimbingSet set, ValueSetID mask) { return ClimbingSet(m_maker.intersect(set.m_set, mask)); }

    // For constants, masks and settled sets.
    const ValueSets& sets() const { return m_maker.sets(); }
    ValueSetMaker& maker() { return m_maker; }
    const SettledValues& settled() const;

    // One node for each (set, name). What is written through a set reaches what is read under the same name through every set that shares an object with it.
    ValueFlowNodeID read(ClimbingSet objects, ValueFlowName);
    ValueFlowNodeID written(ClimbingSet objects, ValueFlowName);
    // The result of a statement of kind Lookup that is made once for each (set, name).
    ValueFlowNodeID lookup(ClimbingSet receivers, ValueFlowName);
    // With a statement of kind CallGroup that is made once for each (set, kind).
    ValueFlowCallGroup group(ClimbingSet callees, ValueFlowCallKind);
    // With a statement of kind Behavior.
    ValueFlowNodeID hub(ValueFlowBehaviorID);
    // One node for each (node, mask): it holds what the mask lets through.
    ValueFlowNodeID masked(ValueFlowNodeID, ValueSetID mask);

    // A node that holds the set for ever: what stands where an operand has to be a node.
    ValueFlowNodeID holding(ClimbingSet);

    void connect(ValueFlowNodeID from, ValueFlowNodeID to);
    void connect(const ValueFlowOperand&, ValueFlowNodeID to);
    void add(ValueFlowNodeID, ClimbingSet);
    void add(ValueFlowNodeID node, ValueSetID constant) { add(node, ClimbingSet(constant)); }
    void reach(FunctionID);
    void mark(ClimbingSet objects, OptionSet<ObjectMutation>);
    void use(BuiltinID, OptionSet<BuiltinUse>);

private:
    friend class ValueFlowSolver;

    void begin(ValueFlowStatementID);
    void end();
    ValueFlowNodeID nodeFor(const ValueFlowNodeKey&);

    const ValueFlowSolver& m_solver;
    ValueFlowRoundLog& m_log;
    ValueSetMaker& m_maker;
    ValueFlowStatementID m_statement;
    ValueSetID m_unknownObjectAlone;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
