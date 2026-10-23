/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTValueFlow.h"
#include "AOTValueSets.h"
#include <optional>
#include <span>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT { namespace Typed {

class ValueFlowProgramNodes;
class ValueFlowRoundLog;
class ValueFlowSolver;
class ValueFlowUniverse;
struct ValueFlowNodeKey;

// A set as it stands in this round; a later round may find more in it.
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

// Where a call has its callee from.
enum class ValueFlowCalleeOrigin : uint8_t {
    Value,
    MemberFoundUnderName,
    OperandOfAppliedEntry,
};

// All that a rule may touch.
class ValueFlowInstancedFunctions;
struct ValueFlowInstancedFunction;

class ValueFlowRuleContext {
    WTF_MAKE_NONCOPYABLE(ValueFlowRuleContext);
public:
    ValueFlowRuleContext(const ValueFlowSolver&, ValueFlowRoundLog&, ValueSetMaker&);

    const Program& program() const;
    const ValueFlowUniverse& universe() const;
    const ValueFlowProgramNodes& nodes() const;
    // LimitID::MaxValueFlowMembersToldApart.
    uint64_t mostMembersToldApart() const;

    // The statement that runs.
    ValueFlowStatementID statement() const { return m_statement; }
    ValueSetID& rememberedByStatement();
    // None for a statement that a rule has made and for one of the host.
    FunctionID functionOfStatement() const;
    ValueID siteOfStatement() const;
    std::span<const ValueFlowOperand> operandsOf(const ValueFlowStatement&) const LIFETIME_BOUND;
    // Of a statement that a rule has made: what lookup(), group() or hub() was asked for.
    ClimbingSet setOfKey() const;
    // The key never moves, so a rule may ask how many they are.
    uint32_t numberOfMembersOfKey() const;
    ValueFlowName nameOfKey() const;
    ValueFlowCallKind callKindOfKey() const;
    // Of a group: none for the group of calls that are not told apart from the other calls of their callees.
    ValueFlowObjectID markerOfKey() const;
    ValueFlowBehaviorID behaviorOfKey() const;

    // The moving state, in the normal form.
    ClimbingSet valueOf(ValueFlowNodeID);
    // Whether valueOf() would answer something. The statement is due again when the node goes from nothing to something, and whatever arrives later passes it by. No node holds nothing.
    bool holdsAnything(ValueFlowNodeID);
    // The set as an operand of a call that a rule makes up: whoever reads the operand sees the set in this very run.
    ValueFlowOperand operandHolding(ClimbingSet set) const { return { { }, set.m_set, ValueSets::anything }; }
    // Through the mask of the operand, by the kind of the mask: where a type stands in the way of a placeholder, the objects of the type's family come in its place.
    ClimbingSet valueOf(const ValueFlowOperand&);
    bool isReached(FunctionID);
    // What the node holds of the members of `within`, without the normal form. The statement is due again only when that part has moved, however much else arrives at the node.
    ClimbingSet valueWithin(ClimbingSet within, ValueFlowNodeID);
    // Whether valueWithin() would answer something. Once it does it does for good: the statement is due again when the first member of the set arrives at the node, and whatever arrives later passes it by.
    bool holdsAnyOf(ClimbingSet within, ValueFlowNodeID);
    // The members of the set that the node does not hold yet.
    ClimbingSet membersMissingFrom(ValueFlowNodeID, ClimbingSet);
    // What valueOf() answers for the node, without what it answered when the last run of this statement asked here.
    ClimbingSet newSince(ValueFlowNodeID);
    void addUnlessHeld(ValueFlowNodeID, ValueFlowObjectID);
    // The members of the set that no run on this thread has been handed here before, under the same number.
    static constexpr unsigned numberOfSetsRememberedByThread = 3;
    ClimbingSet membersNewToThread(ClimbingSet, unsigned number);
    // Whether a run in an earlier round has added exactly this set to the node with add().
    bool wasAddedInEarlierRound(ValueFlowNodeID, ClimbingSet) const;
    // A statement of a body runs for good. One that a rule has made goes to rest when nobody asks for its node any more, and lets go of what it has asked for.
    bool isStatementOfBody() const;
    // The members that have one of the marks so far. The statement is due again only when another of the members gets one.
    ClimbingSet markedAmong(ClimbingSet objects, OptionSet<ObjectMutation>);

    void forEachMember(ClimbingSet set, NOESCAPE const Invocable<void(ValueFlowObjectID)> auto& functor) const { sets().forEachMember(set.m_set, functor); }
    bool contains(ClimbingSet set, ValueFlowObjectID member) const { return sets().contains(set.m_set, member); }
    bool meet(ClimbingSet first, ClimbingSet second) const { return sets().meet(first.m_set, second.m_set); }
    bool meet(ClimbingSet set, ValueSetID constant) const { return sets().meet(set.m_set, constant); }
    ClimbingSet unite(ClimbingSet first, ClimbingSet second) { return ClimbingSet(m_maker.unite(first.m_set, second.m_set)); }
    // The mask is a constant; it may be a complement. No member stands for another here.
    ClimbingSet restrict(ClimbingSet set, ValueSetID mask) { return ClimbingSet(m_maker.intersect(set.m_set, mask)); }

    // For constants and masks.
    const ValueSets& sets() const { return m_maker.sets(); }
    ValueSetMaker& maker() { return m_maker; }

    // One node for each (set, name). What is written through a set reaches what is read under the same name through every set that shares an object with it.
    ValueFlowNodeID read(ClimbingSet objects, ValueFlowName);
    ValueFlowNodeID written(ClimbingSet objects, ValueFlowName);
    // Whether each of the objects is still in the hands of the program alone, followed, and nothing but its properties.
    bool isPlainSoFar(ClimbingSet objects);
    // False while read(objects, a name of the kind) could only answer a node that holds nothing.
    bool mayFindSomethingWritten(ClimbingSet objects, ValueFlowNameKind);
    // In the check of a solve with Rounds::Due: a rule that has a shorter way for a common case leaves it alone.
    bool usesGeneralRulesOnly() const;
    ValueFlowNodeID notPlain();
    ValueFlowNodeID writtenUnderUnknownKey();
    // The result of a statement of kind Lookup that is made once for each (set, name).
    ValueFlowNodeID lookup(ClimbingSet receivers, ValueFlowName);
    // The statement that runs is an operation of a function for which isCodeThatRecordsDescribe() holds.
    bool runsCodeThatRecordsDescribe() const;
    // With a statement of kind CallGroup that is made once for each (set, kind).
    ValueFlowCallGroup group(ClimbingSet callees, ValueFlowCallKind, ValueFlowCalleeOrigin = ValueFlowCalleeOrigin::Value);
    // With a statement of kind Behavior.
    ValueFlowNodeID hub(ValueFlowBehaviorID);
    // One node for each (node, mask, kind): it holds what the mask lets through.
    ValueFlowNodeID masked(ValueFlowNodeID, ValueSetID mask, ValueFlowMaskKind = ValueFlowMaskKind::Plain);

    // A node that holds the set for ever: what stands where an operand has to be a node.
    ValueFlowNodeID holding(ClimbingSet);

    // See ValueFlowInstancedFunctions. Null where the program has none.
    const ValueFlowInstancedFunctions* instancedFunctions() const;
    // Holds the markers of the instances that the function has so far.
    ValueFlowNodeID markersOf(FunctionID);
    void makeInstanceOfOtherCalls(FunctionID);
    // What the calls of one instance pass behind the positional parameters, by the index at which it lies in the array of the rest parameter.
    ValueFlowNodeID restArgumentEntered(FunctionID, unsigned index);
    ValueFlowNodeID restArgumentOfRun(FunctionID, unsigned index);
    // While one of these lives, the statement runs for one instance of its function: wherever the rule names a node of the body, it gets the node of the instance.
    class InstanceOfRun {
        WTF_MAKE_NONCOPYABLE(InstanceOfRun);
    public:
        InstanceOfRun(ValueFlowRuleContext&, FunctionID, ValueFlowObjectID marker);
        ~InstanceOfRun();

    private:
        ValueFlowRuleContext& m_context;
    };
    // While one of these lives, whatever the rule hands to a node of an instanced function goes to the instance with the marker, and a group that it asks for is the group of that instance.
    class InstanceEntered {
        WTF_MAKE_NONCOPYABLE(InstanceEntered);
    public:
        InstanceEntered(ValueFlowRuleContext&, ValueFlowObjectID marker);
        ~InstanceEntered();

    private:
        ValueFlowRuleContext& m_context;
        ValueFlowObjectID m_before;
    };
    // While one of these lives, the statement that runs has these operands.
    class OperandsInPlace {
        WTF_MAKE_NONCOPYABLE(OperandsInPlace);
    public:
        OperandsInPlace(ValueFlowRuleContext&, std::span<const ValueFlowOperand>);
        ~OperandsInPlace();

    private:
        ValueFlowRuleContext& m_context;
        std::span<const ValueFlowOperand> m_before;
    };

    void connect(ValueFlowNodeID from, ValueFlowNodeID to);
    void connect(const ValueFlowOperand&, ValueFlowNodeID to);
    void add(ValueFlowNodeID, ClimbingSet);
    void add(ValueFlowNodeID node, ValueSetID constant) { add(node, ClimbingSet(constant)); }
    void reach(FunctionID);
    void mark(ClimbingSet objects, OptionSet<ObjectMutation>);
    void use(BuiltinID, OptionSet<BuiltinUse>);
    // A read through the unknown object may have answered the built-in. No statement has it as itself: see ValueFlowHostNode::BuiltinsThatStatementsName.
    void useAsAnswerOfUnknownObject(BuiltinID, OptionSet<BuiltinUse>);
    // The statement has met more members than mostMembersToldApart() and takes those past the limit together. The build hears of each such statement once.
    void noteMembersNotToldApart();

    // unknownValue() says here why the rule brings the unknown object into being.
    void noteUnknownSource(ValueFlowUnknownSource source) { m_unknownSource = source; }
    bool leavesOut(ValueFlowUnknownSource) const;
    bool leavesOutSomeUnknownSource() const;
    bool refusesEvaluatedText() const;

    // What the mask of the operand lets through of the value, for whoever has the value of the operand's node from somewhere else: the solution after the solve.
    static ValueSetID throughMaskOf(const ValueFlowOperand&, ValueSetID value, const ValueFlowSolver&, ValueSetMaker&);
    // What valueOf() answers a rule for the operand, for whoever reads the solve once it has ended.
    static ValueSetID valueShownToRules(const ValueFlowOperand&, const ValueFlowSolver&, ValueSetMaker&);

private:
    friend class ValueFlowSolver;

    void begin(ValueFlowStatementID);
    void beginWhereMade(ValueFlowStatementID, const ValueFlowNodeKey&);
    void end();
    const ValueFlowNodeKey& keyOfStatement() const;
    bool hasNeverRun() const { return m_keyOfStatementOfThisTurn || m_isFirstRunOfStatementOfBody; }
    void leaveOutWhatIsKnown();
    ValueFlowNodeID nodeFor(const ValueFlowNodeKey&);
    void addObjectsOfProgram(ValueFlowNodeID, ClimbingSet objects);
    ValueFlowNodeKey keyOfConstant(ValueSetID) const;
    ValueFlowNodeID holdingNothing();
    // What holdingNothing() has answered in this run.
    ValueFlowNodeID m_holdingNothing;
    std::optional<ValueSetID> setOfConstantOfThisTurn(ValueFlowNodeID) const;
public:
    // The members block by block, as forEachMember() gives them one by one. False, and nothing is called, for everything but a set.
    bool forEachBlock(ClimbingSet set, NOESCAPE const Invocable<void(uint64_t nameOfBlock)> auto& functor) const
    {
        if (ValueSets::isComplement(set.m_set))
            return false;
        sets().forEachBlock(set.m_set, functor);
        return true;
    }
private:
    ValueFlowNodeID instanceOf(ValueFlowNodeID, ValueFlowObjectID marker);
    // The node that a rule reads where it names this one, and the node that it hands to.
    ValueFlowNodeID nodeRead(ValueFlowNodeID);
    ValueFlowNodeID nodeHandedTo(ValueFlowNodeID);
    void makeInstance(const ValueFlowInstancedFunction&, ValueFlowObjectID marker);
    void connectAsNamed(ValueFlowNodeID from, ValueFlowNodeID to);

    const ValueFlowSolver& m_solver;
    ValueFlowRoundLog& m_log;
    struct KnownNode {
        uint64_t payloadAndSet { 0 };
        uint32_t kindAndDetail { 0 };
        ValueFlowNodeID node;
    };
    std::array<KnownNode, 4096> m_knownNodes { };
    ValueSetMaker& m_maker;
    ValueFlowStatementID m_statement;
    bool m_isFirstRunOfStatementOfBody { false };
    // Where what the run logs begins.
    size_t m_firstEdgeOfRun { 0 };
    size_t m_firstNodeReadOfRun { 0 };
    size_t m_firstNodeAskedWhetherEmptyOfRun { 0 };
    std::array<ValueSetID, numberOfSetsRememberedByThread> m_rememberedByThread { ValueSets::nothing, ValueSets::nothing, ValueSets::nothing };
    // Of the run.
    std::optional<ValueFlowUnknownSource> m_unknownSource;
    HashMap<uint32_t, ValueSetID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_constantsOfThisTurn;
    // The nodes of the body whose instance the statement runs for; none where it runs for none.
    uint32_t m_firstNodeOfInstance { 0 };
    uint32_t m_endOfNodesOfInstance { 0 };
    ValueFlowObjectID m_markerOfRun;
    ValueFlowObjectID m_markerEntered;
    std::span<const ValueFlowOperand> m_operandsInPlace;
    const ValueFlowNodeKey* m_keyOfStatementOfThisTurn { nullptr };
    ValueSetID m_rememberedByStatementOfThisTurn;
    // By (function, marker). Of the turn: what this thread has logged once in a turn it need not log again.
    HashSet<uint64_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_instancesMadeInTurn;
    uint64_t m_turnOfInstancesMade { 0 };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
