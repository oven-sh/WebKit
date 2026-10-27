/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTFlowUnseenCode.h"
#include "AOTValueFlow.h"
#include "AOTValueSets.h"
#include <optional>
#include <span>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>
#include <wtf/SetForScope.h>

namespace JSC { namespace AOT { namespace Typed {

class ValueFlowInstancedFunctions;
class ValueFlowProgramNodes;
class ValueFlowRoundLog;
class ValueFlowSolver;
class ValueFlowUniverse;
struct ValueFlowInstancedFunction;
struct ValueFlowNodeKey;
enum class ValueFlowRareWrite : uint8_t;

class ClimbingSet {
public:
    ClimbingSet() = default;
    explicit ClimbingSet(ValueSetID constant) : m_set(constant) { }

private:
    friend class ValueFlowRuleContext;

    ValueSetID m_set { ValueSets::nothing };
};

enum class ValueFlowCalleeOrigin : uint8_t {
    Value,
    MemberFoundUnderName,
    OperandOfAppliedEntry,
};

class ValueFlowRuleContext {
    WTF_MAKE_NONCOPYABLE(ValueFlowRuleContext);
public:
    ValueFlowRuleContext(const ValueFlowSolver&, ValueFlowRoundLog&, ValueSetMaker&);

    const Program& program() const;
    const ValueFlowUniverse& universe() const;
    const ValueFlowProgramNodes& nodes() const;
    uint64_t mostMembersToldApart() const;

    ValueFlowStatementID statement() const { return m_statement; }
    ValueSetID& rememberedByStatement();
    FunctionID functionOfStatement() const;
    std::span<const ValueFlowOperand> operandsOf(const ValueFlowStatement&) const LIFETIME_BOUND;
    ClimbingSet setOfKey() const;
    uint32_t numberOfMembersOfKey() const;
    ValueFlowName nameOfKey() const;
    ValueFlowCallKind callKindOfKey() const;
    ValueFlowObjectID markerOfKey() const;

    ClimbingSet valueOf(ValueFlowNodeID);
    bool holdsAnything(ValueFlowNodeID);
    ValueFlowOperand operandHolding(ClimbingSet set) const { return { { }, set.m_set, ValueSets::anything }; }
    ClimbingSet valueOf(const ValueFlowOperand&);
    bool isReached(FunctionID);
    ClimbingSet valueWithin(ClimbingSet within, ValueFlowNodeID);
    ClimbingSet valueWithinAskedLast(ClimbingSet within, ValueFlowNodeID);
    bool holdsAnyOf(ClimbingSet within, ValueFlowNodeID);
    ClimbingSet membersMissingFrom(ValueFlowNodeID, ClimbingSet);
    ClimbingSet newSince(ValueFlowNodeID);
    void addUnlessHeld(ValueFlowNodeID, ValueFlowObjectID);
    static constexpr unsigned numberOfSetsRememberedByThread = 3;
    ClimbingSet membersNewToThread(ClimbingSet, unsigned number);
    bool wasAddedInEarlierRound(ValueFlowNodeID, ClimbingSet) const;
    bool isStatementOfBody() const;
    ClimbingSet markedAmong(ClimbingSet objects, OptionSet<ObjectMutation>);

    void forEachMember(ClimbingSet set, NOESCAPE const Invocable<void(ValueFlowObjectID)> auto& functor) const { sets().forEachMember(set.m_set, functor); }
    bool forEachBlock(ClimbingSet set, NOESCAPE const Invocable<void(uint64_t nameOfBlock)> auto& functor) const
    {
        if (ValueSets::isComplement(set.m_set))
            return false;
        sets().forEachBlock(set.m_set, functor);
        return true;
    }
    bool contains(ClimbingSet set, ValueFlowObjectID member) const { return sets().contains(set.m_set, member); }
    bool holdsOneMember(ClimbingSet set) const { return !!sets().onlyMember(set.m_set); }
    bool holdsMoreMembersThan(ClimbingSet set, uint32_t number) const { return !ValueSets::isComplement(set.m_set) && sets().size(set.m_set) > number; }
    bool holdsAllOf(ClimbingSet set, ClimbingSet members) const { return sets().isSubset(members.m_set, set.m_set); }
    void appendPartsWithChainsOfTheirOwn(ClimbingSet set, Vector<ValueSetID>& parts) { m_maker.appendParts(set.m_set, parts); }
    bool meet(ClimbingSet first, ClimbingSet second) const { return m_maker.meet(first.m_set, second.m_set); }
    bool meet(ClimbingSet set, ValueSetID constant) const { return m_maker.meet(set.m_set, constant); }
    ClimbingSet unite(ClimbingSet first, ClimbingSet second) { return ClimbingSet(m_maker.unite(first.m_set, second.m_set)); }
    ValueSetID withMembersOf(ValueSetID constant, ClimbingSet set) { return m_maker.unite(constant, set.m_set); }
    ClimbingSet restrict(ClimbingSet set, ValueSetID mask) { return ClimbingSet(m_maker.intersect(set.m_set, mask)); }

    const ValueSets& sets() const { return m_maker.sets(); }
    ValueSetMaker& maker() { return m_maker; }

    ValueFlowNodeID read(ClimbingSet objects, ValueFlowName);
    ValueFlowNodeID written(ClimbingSet objects, ValueFlowName);
    bool mayFindSomethingWritten(ClimbingSet objects, ValueFlowNameKind);
    std::optional<ClimbingSet> holdersUnder(ValueFlowName, ClimbingSet within);
    ClimbingSet holdersOfLibraryExportsWrittenUnder(PropertyKeyID);
    bool mayFindWhatIsWrittenUnderUnknownKey(ValueFlowName) const;
    bool usesGeneralRulesOnly() const;
    ValueFlowNodeID lookup(ClimbingSet receivers, ValueFlowName);
    bool runsCodeThatRecordsDescribe() const;
    ValueFlowCallGroup group(ClimbingSet callees, ValueFlowCallKind, ValueFlowCalleeOrigin = ValueFlowCalleeOrigin::Value);
    ValueFlowNodeID hub(ValueFlowBehaviorID);
    ValueFlowNodeID masked(ValueFlowNodeID, ValueSetID mask, ValueFlowMaskKind = ValueFlowMaskKind::Plain);

    ValueFlowNodeID holding(ClimbingSet);

    const ValueFlowInstancedFunctions* instancedFunctions() const { return m_instancedFunctions; }
    ValueFlowNodeID markersOf(FunctionID);
    void makeInstanceOfOtherCalls(FunctionID);
    ValueFlowNodeID restArgumentEntered(FunctionID, unsigned index);
    ValueFlowNodeID restArgumentOfRun(FunctionID, unsigned index);
    class InstanceOfRun {
        WTF_MAKE_NONCOPYABLE(InstanceOfRun);
    public:
        InstanceOfRun(ValueFlowRuleContext&, FunctionID, ValueFlowObjectID marker);
        ~InstanceOfRun();

    private:
        ValueFlowRuleContext& m_context;
    };
    class InstanceEntered : SetForScope<ValueFlowObjectID> {
    public:
        InstanceEntered(ValueFlowRuleContext& context, ValueFlowObjectID marker) : SetForScope(context.m_markerEntered, marker), m_context(context) { ++context.m_runEpoch; }
        ~InstanceEntered() { ++m_context.m_runEpoch; }

    private:
        ValueFlowRuleContext& m_context;
    };
    class OperandsInPlace : SetForScope<std::span<const ValueFlowOperand>> {
    public:
        OperandsInPlace(ValueFlowRuleContext& context, std::span<const ValueFlowOperand> operands) : SetForScope(context.m_operandsInPlace, operands) { }
    };

    void connect(ValueFlowNodeID from, ValueFlowNodeID to);
    void connect(const ValueFlowOperand&, ValueFlowNodeID to);
    void add(ValueFlowNodeID, ClimbingSet);
    void add(ValueFlowNodeID node, ValueSetID constant) { add(node, ClimbingSet(constant)); }
    void reach(FunctionID);
    void mark(ClimbingSet objects, OptionSet<ObjectMutation>);
    void use(BuiltinID, OptionSet<BuiltinUse>);
    void useAsAnswerOfUnknownObject(BuiltinID, OptionSet<BuiltinUse>);
    void noteMembersNotToldApart();

    void noteUnknownSource(ValueFlowUnknownSource source) { m_unknownSource = source; }
    bool leavesOut(ValueFlowUnknownSource) const;
    bool leavesOutSomeUnknownSource() const;
    RefusedCode refusedCode() const;
    bool refusesEvaluatedText() const { return refusedCode().evaluatedText; }
    bool isGuardedByRemovedFeature(BuiltinID) const;
    bool isSabotaged(Sabotage) const;

    static ValueSetID throughMaskOf(const ValueFlowOperand&, ValueSetID value, const ValueFlowSolver&, ValueSetMaker&);
    static ValueSetID valueShownToRules(const ValueFlowOperand&, const ValueFlowSolver&, ValueSetMaker&);

private:
    friend class ValueFlowSolver;

    void begin(ValueFlowStatementID);
    void beginWhereMade(ValueFlowStatementID, const ValueFlowNodeKey&, uint32_t madeNodes);
    void end();
    const ValueFlowNodeKey& keyOfStatement() const;
    bool hasNeverRun() const { return m_keyOfStatementOfThisTurn || m_isFirstRunOfStatementOfBody; }
    void leaveOutWhatIsKnown();
    ValueFlowNodeID nodeFor(const ValueFlowNodeKey&);
    struct KnownNode;
    ValueFlowNodeID nodeForUncached(const ValueFlowNodeKey&, KnownNode&, uint32_t hash);
    ValueFlowNodeID constantNodeForEdge(const ValueFlowNodeKey&);
    ValueFlowNodeID notPlain();
    ValueFlowNodeID writtenUnderUnknownKey();
    ValueFlowNodeID rareWriteNode(ValueFlowRareWrite);
    ClimbingSet objectsThatMayHoldSomethingUnder(ValueFlowName, ClimbingSet objects);
    ValueFlowNodeID mutationNode(ObjectMutation);
    void logMadeNodesAskedFor(const ValueFlowNodeKey&, uint32_t madeNodes);
    void addObjectsOfProgram(ValueFlowNodeID, ClimbingSet objects);
    ValueFlowNodeKey keyOfConstant(ValueSetID) const;
    ValueFlowNodeID holdingNothing();
    ValueFlowNodeID m_holdingNothing;
    std::optional<ValueSetID> setOfConstantOfThisTurn(ValueFlowNodeID) const;
    ValueFlowNodeID instanceOf(ValueFlowNodeID, ValueFlowObjectID marker);
    ValueFlowNodeID nodeRead(ValueFlowNodeID);
    ValueFlowNodeID nodeHandedTo(ValueFlowNodeID);
    ValueFlowNodeID instanceHandedTo(ValueFlowNodeID);
    void makeInstance(const ValueFlowInstancedFunction&, ValueFlowObjectID marker);
    void connectAsNamed(ValueFlowNodeID from, ValueFlowNodeID to);
    bool isRedundantAdd(ValueFlowNodeID, ValueSetID);

    struct RunCacheEntry {
        uint64_t key { 0 };
        uint64_t epochAndValue { 0 };
    };
    using RunCache = std::array<RunCacheEntry, 64>;
    static RunCacheEntry& runCacheEntry(RunCache& cache, uint64_t key) { return cache[key * 0x9e3779b97f4a7c15ull >> 58]; }
    uint64_t addEpoch() const { return m_runEpoch << 8 | (m_unknownSource ? static_cast<uint64_t>(*m_unknownSource) + 1 : 0); }

    uint64_t m_runEpoch { 0 };
    RunCache m_edgeCache { };
    RunCache m_addCache { };
    RunCache m_holdsAnyOfCache { };
    const ValueFlowSolver& m_solver;
    ValueFlowRoundLog& m_log;
    const ValueFlowInstancedFunctions* const m_instancedFunctions;
    const bool m_skipsRedundantAdds;
    struct alignas(32) KnownNode {
        uint64_t payloadAndSet { 0 };
        uint32_t kindAndDetail { 0 };
        ValueFlowNodeID node;
        uint32_t madeNodes { 0 };
    };
    std::array<KnownNode, 2048> m_knownNodes { };
    struct ConstantOfTurn {
        uint64_t detailAndSet { 0 };
        uint32_t turn { 0 };
        ValueFlowNodeID node;
    };
    std::array<ConstantOfTurn, 256> m_constantsOfTurn { };
    std::array<ValueFlowNodeID, 32> m_mutationNodes { };
    ValueSetMaker& m_maker;
    ValueFlowStatementID m_statement;
    uint32_t m_madeNodesOfStatement { UINT32_MAX };
    bool m_isFirstRunOfStatementOfBody { false };
    bool m_filtersKnownEdges { true };
    size_t m_firstEdgeOfRun { 0 };
    size_t m_firstNodeReadOfRun { 0 };
    size_t m_firstNodeAskedWhetherEmptyOfRun { 0 };
    size_t m_firstNameWatchedOfRun { 0 };
    std::array<ValueSetID, numberOfSetsRememberedByThread> m_rememberedByThread { ValueSets::nothing, ValueSets::nothing, ValueSets::nothing };
    std::optional<ValueFlowUnknownSource> m_unknownSource;
    HashMap<uint32_t, ValueSetID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_constantsOfThisTurn;
    uint32_t m_firstNodeOfInstance { 0 };
    uint32_t m_endOfNodesOfInstance { 0 };
    ValueFlowObjectID m_markerOfRun;
    ValueFlowObjectID m_markerEntered;
    std::span<const ValueFlowOperand> m_operandsInPlace;
    const ValueFlowNodeKey* m_keyOfStatementOfThisTurn { nullptr };
    ValueSetID m_rememberedByStatementOfThisTurn;
    HashSet<uint64_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_instancesMadeInTurn;
    uint64_t m_turnOfInstancesMade { 0 };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
