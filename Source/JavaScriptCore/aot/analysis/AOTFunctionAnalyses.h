/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTAnalysisContext.h"
#include "AOTBlockCycles.h"
#include "AOTBlockFrequencies.h"
#include "AOTCFG.h"
#include "AOTChangeLog.h"
#include "AOTDiagnostics.h"
#include "AOTDominators.h"
#include "AOTEffectSummaries.h"
#include "AOTEffectsCache.h"
#include "AOTEscapeAnalysis.h"
#include "AOTGuardAvailability.h"
#include "AOTInductionVariables.h"
#include "AOTLiveness.h"
#include "AOTNaturalLoops.h"
#include "AOTPassID.h"
#include "AOTUseEffectHistory.h"
#include "AOTUseLists.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include <array>
#include <optional>
#include <span>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT { namespace Typed {

struct TypingMemo;

enum class FunctionAnalysis : uint16_t {
    ControlFlow = 1 << 0,
    Dominators = 1 << 1,
    NaturalLoops = 1 << 2,
    Cycles = 1 << 3,
    UseLists = 1 << 4,
    Identities = 1 << 5,
    Liveness = 1 << 6,
    Effects = 1 << 7,
    Escape = 1 << 8,
    EscapeHistory = 1 << 9,
    Frequencies = 1 << 10,
    Guards = 1 << 11,
    FailedTests = 1 << 12,
    Induction = 1 << 13,
};

using AnalysisSet = OptionSet<FunctionAnalysis>;

inline constexpr AnalysisSet valueAnalyses {
    FunctionAnalysis::UseLists,
    FunctionAnalysis::Identities,
    FunctionAnalysis::Liveness,
    FunctionAnalysis::Effects,
    FunctionAnalysis::Escape,
    FunctionAnalysis::EscapeHistory,
    FunctionAnalysis::Frequencies,
    FunctionAnalysis::Guards,
    FunctionAnalysis::FailedTests,
    FunctionAnalysis::Induction,
};

inline constexpr AnalysisSet analysesThatReadNoTypes {
    FunctionAnalysis::UseLists,
    FunctionAnalysis::Identities,
    FunctionAnalysis::Liveness,
};

inline constexpr AnalysisSet controlFlowAnalyses {
    FunctionAnalysis::ControlFlow,
    FunctionAnalysis::Dominators,
    FunctionAnalysis::NaturalLoops,
    FunctionAnalysis::Cycles,
};

inline constexpr AnalysisSet allAnalyses = valueAnalyses | controlFlowAnalyses;

inline constexpr unsigned numberOfFunctionAnalyses = 14;

class FunctionAnalyses {
    WTF_MAKE_NONCOPYABLE(FunctionAnalyses);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(FunctionAnalyses);
public:
    FunctionAnalyses(const AnalysisContext&, const Function&);
    FunctionAnalyses(const AnalysisContext&, Function&);
    ~FunctionAnalyses();

    const AnalysisContext& context() const LIFETIME_BOUND { return m_context; }
    const EffectContext& effectContext() const LIFETIME_BOUND { return m_effectContext; }
    const Program& program() const { return m_context.program; }
    const Function& function() const { return m_function; }
    Subject subject(ValueID = { }) const;
    ChangeLog& changes() LIFETIME_BOUND
    {
        RELEASE_ASSERT(m_changes);
        return *m_changes;
    }

    CFG& cfg() LIFETIME_BOUND;
    Dominators& dominators() LIFETIME_BOUND;
    NaturalLoops& naturalLoops() LIFETIME_BOUND;
    const BlockCycles& blockCycles() LIFETIME_BOUND;
    const UseLists& useLists() LIFETIME_BOUND;
    const UseLists* useListsIfUpToDate() const LIFETIME_BOUND { return m_values.useLists && m_values.useLists->isUpToDate() && !m_values.didDropBesideUseLists ? &*m_values.useLists : nullptr; }
    UseLists* updatingUseLists() LIFETIME_BOUND;
    void setEveryChangeIsReported(bool isReported) { m_everyChangeIsReported = isReported; }
    // While an SSARepair holds two definitions of a variable and has not run, a use may name a value that does not reach it.
    void didBeginRepair() { ++m_numberOfPendingRepairs; }
    void didEndRepair() { --m_numberOfPendingRepairs; }
    void willFollowValuesThroughParameters() const
    {
        if (m_numberOfPendingRepairs) [[unlikely]]
            reportQuestionBeforeRepair();
    }
    const ValueIdentities& identities() LIFETIME_BOUND;
    const Liveness& liveness() LIFETIME_BOUND;
    const EscapeAnalysis& escape() LIFETIME_BOUND;
    const EscapeAnalysis& escapeWithLiveInstances() LIFETIME_BOUND;
    const UseEffectHistory& escapeHistory() LIFETIME_BOUND;
    const BlockFrequencies& blockFrequencies() LIFETIME_BOUND;
    const GuardAvailability& guards() LIFETIME_BOUND;
    const OpcodeSet& opcodesOfValues() LIFETIME_BOUND;
    bool hasPristineTest() { return opcodesOfValues().contains(Opcode::IsPristine); }
    const InductionVariables& inductionVariables() LIFETIME_BOUND;

    const EffectsOnEdges& effectsOf(ValueID id) LIFETIME_BOUND { return effectsCache().of(id); }
    const EffectsOnEdges& effectsOfTerminator(BlockID block) LIFETIME_BOUND { return effectsCache().ofTerminator(block); }
    EffectTraitsOnEdges effectTraitsOfTerminator(BlockID block) { return effectsCache().traitsOfTerminator(block); }
    bool mayThrow(ValueID id) { return answerFromTraits<&EffectTraits::mayThrow, &EffectTraitsOnEdges::normal>(id); }
    bool maySuspend(ValueID id) { return answerFromTraits<&EffectTraits::maySuspend, &EffectTraitsOnEdges::normal>(id); }
    bool mayCallUnknownCode(ValueID id) { return answerFromTraits<&EffectTraits::mayCallUnknownCode, &EffectTraitsOnEdges::normal>(id); }
    bool maySuspendOnAnyEdge(ValueID id) { return answerFromTraits<&EffectTraits::maySuspend, &EffectTraitsOnEdges::anyEdge>(id); }
    bool mayRunOtherCodeOnAnyEdge(ValueID id) { return answerFromTraits<&EffectTraits::mayRunOtherCode, &EffectTraitsOnEdges::anyEdge>(id); }
    bool mayWriteWhatIsSeen(ValueID id) { return answerFromTraits<&EffectTraits::mayWriteWhatIsSeen, &EffectTraitsOnEdges::anyEdge>(id); }
    bool hasNoEffects(ValueID id) { return answerFromTraits<&EffectTraits::isPure, &EffectTraitsOnEdges::anyEdge>(id); }
    bool isRemovableWhenUnused(ValueID id) { return answerFromTraits<&EffectTraits::isRemovableWhenUnused, &EffectTraitsOnEdges::anyEdge>(id); }
    bool isUnobservable(ValueID id) { return answerFromTraits<&EffectTraits::isUnobservable, &EffectTraitsOnEdges::anyEdge>(id); }

    const GuardFacts& guardFacts(ValueID guard) LIFETIME_BOUND { return guards().factsOf(guard); }
    GuardState guardStateAt(ValueID guard, const Use&);
    bool isGuardAvailable(ValueID guard, ProgramPoint point) { return guards().stateAt(guard, point) == GuardState::Available; }
    TypingMemo* typingMemo() const { return m_typingMemo; }
    void setTypingMemo(TypingMemo* memo) { m_typingMemo = memo; }

    EarlierTest findEarlierTest(const Value& test, BlockID at) { return failedTests().find(test, at); }
    bool hasFailedTestOf(ValueID value) { return failedTests().hasTestOf(value); }

    ExecutionCount executionCountOf(BlockID);
    bool isInCycle(BlockID block) { return cfg().hasCycle() && blockCycles().isInCycle(block); }

    ValueID identityOf(ValueID);
    ValueID sameObjectThroughCasts(ValueID id) const { return Typed::sameObjectThroughCasts(m_function, id); }
    ValueID definitionThroughUpcasts(ValueID id) const { return Typed::definitionThroughUpcasts(m_function, id); }

    bool dominates(ValueID definition, ProgramPoint);
    bool dominates(ValueID definition, const Use& use) { return dominates(definition, useLists().pointOf(use)); }
    bool dominates(Edge, BlockID);

    template<typename Problem> Vector<std::optional<typename Problem::State>> solveForward(Problem&);

    void invalidate(AnalysisSet requested) { invalidate(requested, { }); }
    AnalysisSet computed() const;
    void didChangeValues() { invalidate(valueAnalyses); }
    void didChangeTypes() { invalidate(valueAnalyses - analysesThatReadNoTypes); }
    void didRetypeValues();
    void didChangeControlFlow() { invalidate(allAnalyses); }

    void didAddDetachedValue(ValueID);
    void didListValues(BlockID block, std::span<const ValueID>) { didChangeBlock(block); }
    void didEraseValues(BlockID, std::span<const ValueID> erased);
    void didChangeBlock(BlockID);
    void didReplaceUses(ValueID from, ValueID to);
    void didChangeValuesButNotUses();
    void didChangeControlFlow(std::span<const BlockID> changedBlocks);
    void didRenumber(const Renumbering&);

    // False: an answer that they hold is not what the function says, which has been reported.
    bool verifyCached();

    void setRunningPass(PassID pass)
    {
        m_runningPass = pass;
        if (m_values.identities && !m_values.identities->isComplete())
            completeIdentitiesBeforePass();
    }
    void didRunPass()
    {
        if (m_values.useLists)
            m_values.didDropBesideUseLists = true;
    }
    static void logBuilds();

    std::span<const Edge> edgesFromUnreachableBlocks() LIFETIME_BOUND;
    bool isEnteredFromUnreachableBlock(BlockID);
    bool hasSingleIncomingEdge(BlockID block) { return cfg().singleIncomingEdge(block) && !isEnteredFromUnreachableBlock(block); }

private:
    void invalidate(AnalysisSet requested, AnalysisSet broughtUpToDate);
    friend class EscapeAnalysis;

    EffectsCache& effectsCache();

    template<bool (EffectTraits::*question)() const, EffectTraits EffectTraitsOnEdges::*edge>
    bool answerFromTraits(ValueID id)
    {
        const EffectTraitBounds& bounds = effectFactsOf(m_function.at(id).opcode).traits;
        bool answer = (bounds.atLeast.*question)();
        if (answer == (bounds.atMost.*question)())
            return answer;
        return (effectsCache().traitsOf(id).*edge.*question)();
    }
    EscapeAnalysisInputs inputsOfEscapeAnalysis();
    const FailedTests& failedTests();

    struct ControlFlowResults {
        size_t numberOfBlocks { 0 };
        bool mayDescribeFormerGraph { false };
        bool isReadByUseLists { false };
        std::optional<CFG> cfg;
        std::optional<Vector<Edge>> edgesFromUnreachableBlocks;
        std::optional<Dominators> dominators;
        std::optional<NaturalLoops> naturalLoops;
        std::optional<BlockCycles> blockCycles;
    };

    struct ValueResults {
        size_t numberOfValues { 0 };
        bool mayHoldResults { false };
        std::optional<UseLists> useLists;
        bool didDropBesideUseLists { false };
        std::optional<ValueIdentities> identities;
        bool didUpdateIdentities { false };
        std::optional<Liveness> liveness;
        bool hasEffects { false };
        std::optional<EscapeAnalysis> escape;
        std::optional<UseEffectHistory> escapeHistory;
        std::optional<BlockFrequencies> blockFrequencies;
        bool frequenciesMayDescribeFormerFunction { false };
        std::optional<GuardAvailability> guards;
        std::optional<OpcodeSet> opcodes;
        size_t numberOfValuesWithCollectedOpcodes { 0 };
        std::optional<FailedTests> failedTests;
        std::optional<InductionVariables> inductionVariables;
    };

    void checkBlockCount() const { RELEASE_ASSERT(m_function.blocks.size() == m_controlFlow.numberOfBlocks); }
    void checkValueCount() const { RELEASE_ASSERT(m_function.values.size() == m_values.numberOfValues); }
    void willComputeValueAnalysis();
    template<typename Result> const Result& valueResult(std::optional<Result>&, const Invocable<void()> auto& make);
    template<typename... Arguments> AnalysisSet tellUseLists(void (UseLists::*)(Arguments...), std::type_identity_t<Arguments>...);
    void drop(AnalysisSet);
    void dropResultsOfFormerGraph();
    void dropFrequencies();
    void dropFrequenciesOfFormerFunction();
    AnalysisSet heldControlFlowAnalyses() const;
    void didBuild(FunctionAnalysis);
    void didBringUseListsUpToDate(const UseLists::Update&);
    void verifyUseLists() const;
    void completeIdentitiesBeforePass();
    void verifyIdentities(const CFG&) const;
    void verifyLiveness() const;
    void verifyFrequencies();
    void willDrop(AnalysisSet);
    void reportQuestionBeforeRepair() const;

    const AnalysisContext m_context;
    const Function& m_function;
    TypingMemo* m_typingMemo { nullptr };
    ChangeLog m_ownChanges;
    ChangeLog* m_changes { nullptr };
    Function* m_functionWithOwnChanges { nullptr };
    std::optional<EffectSummaries> m_unknownSummaries;
    const EffectContext m_effectContext;
    EffectsCache m_effects;
    ControlFlowResults m_controlFlow;
    ValueResults m_values;
    const bool m_countsBuilds;
    const bool m_verifiesUpdates;
    PassID m_runningPass { PassID::None };
    bool m_everyChangeIsReported { false };
    unsigned m_numberOfPendingRepairs { 0 };
    std::array<PassID, numberOfFunctionAnalyses> m_passesThatDropped { };
};

ASCIILiteral sectionNameOf(FunctionAnalysis);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
