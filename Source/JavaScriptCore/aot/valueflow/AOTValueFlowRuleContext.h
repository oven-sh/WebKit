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
#include <array>
#include <optional>
#include <span>
#include <wtf/HashMap.h>
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

struct ValueFlowObjectsToldApart {
    Vector<ValueSetID, 16> groups;
    ValueSetID rest { ValueSets::nothing };
};

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
    std::span<const ValueFlowOperand> operandsOf(const ValueFlowStatement&) LIFETIME_BOUND;
    bool canBeReached(const ValueFlowStatement&);
    ClimbingSet setOfKey() const;
    uint32_t numberOfMembersOfKey() const;
    ValueFlowName nameOfKey() const;
    bool isLookupForCall() const;
    ValueFlowCallKind callKindOfKey() const;
    ValueFlowObjectID markerOfKey() const;

    ClimbingSet valueOf(ValueFlowNodeID);
    // The proxies in the value of the first operand of the statement whose handler flow follows. The rule that asks calls their trap, and hands the operation on to their target.
    ClimbingSet proxiesAskedByTheirTrapsIn(const ValueFlowOperand&);
    // A rule that knows that it does more to what it was handed says so at once. Nothing hangs on it: what no rule says the solver finds when all is quiet.
    void openProxiesAmong(ClimbingSet);
    ClimbingSet followedProxiesAmong(ClimbingSet);
    void noteThatStatementAsksProxiesOfConstructorByTheirTraps();
    // A proxy that hands on what comes from below holds nothing itself: whoever walks up finds what stands above it.
    ClimbingSet withoutProxiesThatWalksPass(ClimbingSet);
    ClimbingSet proxiesThatWalksPassAmong(ClimbingSet);
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
    bool isFirstToTieNamespaceInThisRound(ModuleID);
    bool isStatementOfBody() const;
    ClimbingSet markedAmong(ClimbingSet objects, OptionSet<ObjectMutation>);

    void forEachMember(ClimbingSet set, NOESCAPE const Invocable<void(ValueFlowObjectID)> auto& functor) const { sets().forEachMember(set.m_set, functor); }
    bool anyMember(ClimbingSet set, NOESCAPE const Invocable<bool(ValueFlowObjectID)> auto& functor) const { return sets().anyMember(set.m_set, functor); }
    // For a walk that would take the same step again.
    bool holdSameMembers(ClimbingSet first, ClimbingSet second) const { return first.m_set == second.m_set; }
    // What lies above objects as they are made hangs on the objects and on nothing else: the thread remembers it under the latest set of each slot.
    ValueSetID prototypesAtCreationRememberedOf(ClimbingSet objects, NOESCAPE const Invocable<ValueSetID()> auto& find)
    {
        PrototypesAtCreation& slot = m_prototypesAtCreationOfSets[(objects.m_set.index() * 0x9e3779b1u) >> 24];
        if (slot.objects != objects.m_set)
            slot = { objects.m_set, find() };
        return slot.prototypes;
    }
    // Which closures are handed a receiver hangs on their functions and on nothing else: the thread remembers them under the latest set.
    // n calls of a member that may be any of n closures ask about the same set.
    std::span<const ValueFlowObjectID> closuresTakingReceiverRememberedOf(ClimbingSet closures, NOESCAPE const Invocable<bool(ValueFlowObjectID)> auto& takesReceiver) LIFETIME_BOUND
    {
        if (!m_closuresTakingReceiver.isFilled || m_closuresTakingReceiver.closures != closures.m_set) {
            m_closuresTakingReceiver.isFilled = true;
            m_closuresTakingReceiver.closures = closures.m_set;
            m_closuresTakingReceiver.members.shrink(0);
            sets().forEachMember(closures.m_set, [&](ValueFlowObjectID closure) {
                if (takesReceiver(closure))
                    m_closuresTakingReceiver.members.append(closure);
            });
        }
        return m_closuresTakingReceiver.members.span();
    }
    // How a set is told apart hangs on its members and on whether all else is at rest: the thread remembers the latest answer.
    // n calls of a method on any of n objects ask about the same set, and listing the set takes n.
    const ValueFlowObjectsToldApart& objectsToldApartRememberedOf(ClimbingSet objects, bool isAllElseAtRest, NOESCAPE const Invocable<ValueFlowObjectsToldApart()> auto& find) LIFETIME_BOUND
    {
        ObjectsToldApart& known = m_objectsToldApart;
        if (!known.isFilled || known.objects != objects.m_set || known.isAllElseAtRest != isAllElseAtRest) {
            known.isFilled = true;
            known.objects = objects.m_set;
            known.isAllElseAtRest = isAllElseAtRest;
            known.answer = find();
        }
        return known.answer;
    }
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
    uint32_t numberOfMembers(ClimbingSet set) const { return sets().size(set.m_set); }
    void appendPartsWithChainsOfTheirOwn(ClimbingSet set, Vector<ValueSetID>& parts) { m_maker.appendParts(set.m_set, parts); }
    bool meet(ClimbingSet first, ClimbingSet second) const { return m_maker.meet(first.m_set, second.m_set); }
    bool meet(ClimbingSet set, ValueSetID constant) const { return m_maker.meet(set.m_set, constant); }
    ClimbingSet unite(ClimbingSet first, ClimbingSet second) { return ClimbingSet(m_maker.unite(first.m_set, second.m_set)); }
    ValueSetID withMembersOf(ValueSetID constant, ClimbingSet set) { return m_maker.unite(constant, set.m_set); }
    ClimbingSet restrict(ClimbingSet set, ValueSetID mask) { return ClimbingSet(m_maker.intersect(set.m_set, mask)); }

    const ValueSets& sets() const { return m_maker.sets(); }
    ValueSetMaker& maker() { return m_maker; }

    ValueFlowNodeID read(ClimbingSet objects, ValueFlowCellName cell) { return readWhatWasWritten(objects, static_cast<ValueFlowName>(cell)); }
    ValueFlowNodeID readWhatWasWritten(ClimbingSet objects, ValueFlowName);
    ValueFlowNodeID readWhatWasWritten(ClimbingSet objects, ValueFlowCellName) = delete;
    // The value of that node if an earlier turn made it. The statement does not ask for the node, so a change of the value does not run it again.
    std::optional<ClimbingSet> peekAtWhatWasWritten(ClimbingSet objects, ValueFlowName);
    // What peeks of this turn found from these objects on. The thread remembers it: no value changes within a turn.
    std::optional<ClimbingSet> sourcesOfCopiesSeenFrom(ClimbingSet objects);
    void noteSourcesOfCopiesSeenFrom(ClimbingSet objects, ClimbingSet seen);
    ValueFlowNodeID written(ClimbingSet objects, ValueFlowName);
    bool mayFindSomethingWritten(ClimbingSet objects, ValueFlowNameKind);
    std::optional<ClimbingSet> holdersUnder(ValueFlowName, ClimbingSet within);
    ClimbingSet holdersOfLibraryExportsWrittenUnder(PropertyKeyID);
    bool mayFindWhatIsWrittenUnderUnknownKey(ValueFlowName) const;
    bool usesGeneralRulesOnly() const;
    enum class LookupUse : bool { Any, OnlyCalledOnReceiver };
    ValueFlowNodeID lookup(ClimbingSet receivers, ValueFlowName, LookupUse = LookupUse::Any);
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
    // The function of the library under the number, or nothing. Where a rule wants to enter it and the program does not hold it, whoever handed the program over hears of it.
    FunctionID libraryFunctionToEnter(RuntimeFunctionID);

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
    struct PrototypesAtCreation {
        ValueSetID objects;
        ValueSetID prototypes;
    };
    std::array<PrototypesAtCreation, 256> m_prototypesAtCreationOfSets { };
    struct ClosuresTakingReceiver {
        bool isFilled { false };
        ValueSetID closures;
        Vector<ValueFlowObjectID> members;
    };
    ClosuresTakingReceiver m_closuresTakingReceiver;
    struct ObjectsToldApart {
        bool isFilled { false };
        bool isAllElseAtRest { false };
        ValueSetID objects;
        ValueFlowObjectsToldApart answer;
    };
    ObjectsToldApart m_objectsToldApart;
    RunCache m_edgeCache { };
    RunCache m_addCache { };
    RunCache m_holdsAnyOfCache { };
    const ValueFlowSolver& m_solver;
    ValueFlowRoundLog& m_log;
    const ValueFlowInstancedFunctions* const m_instancedFunctions;
    // What is seen at a node goes by the edges that lead to it, so every solve skips the same adds. Marks are read as sets alone: a solve that logs records each statement that marks.
    const bool m_skipsRedundantMarks;
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
    // One list for each statement whose operands a run has narrowed. A rule keeps its span while it runs rules for other statements.
    Vector<std::unique_ptr<Vector<ValueFlowOperand, 8>>, 2> m_narrowedOperandsOfRun;
    const ValueFlowNodeKey* m_keyOfStatementOfThisTurn { nullptr };
    ValueSetID m_rememberedByStatementOfThisTurn;
    HashSet<uint64_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_instancesMadeInTurn;
    uint64_t m_turnOfInstancesMade { 0 };
    uint64_t m_turnOfSourcesOfCopiesSeen { 0 };
    UncheckedKeyHashMap<uint32_t, ValueSetID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_sourcesOfCopiesSeenFrom;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
