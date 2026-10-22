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
#include <array>
#include <optional>
#include <span>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>
#include <wtf/Vector.h>

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
    // Of a function that nobody edits while this lives, or whose edits this is told of as before there was a log. Other threads may read the function meanwhile.
    FunctionAnalyses(const AnalysisContext&, const Function&);
    // Of a function that this thread may edit. It gets a log for as long as this lives, unless it has one.
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
    // The lists that are held, for whoever tells them which blocks it has changed (UseLists::didChangeBlock() and its neighbors). Null where none are held, and wherever the
    // running pass is not known to tell of every change: there the lists are dropped with the rest.
    // They take note, and useLists() brings them up to date when they are next asked for. Whoever has told them, and nobody else, leaves them out of what it invalidates.
    UseLists* updatingUseLists() LIFETIME_BOUND;
    // For a stretch of a pass that is not known to tell of every change: while this holds, whoever edits the function does.
    void setEveryChangeIsReported(bool isReported) { m_everyChangeIsReported = isReported; }
    const ValueIdentities& identities() LIFETIME_BOUND;
    const Liveness& liveness() LIFETIME_BOUND;
    const EscapeAnalysis& escape() LIFETIME_BOUND;
    const EscapeAnalysis& escapeWithLiveInstances() LIFETIME_BOUND;
    const UseEffectHistory& escapeHistory() LIFETIME_BOUND;
    const BlockFrequencies& blockFrequencies() LIFETIME_BOUND;
    const GuardAvailability& guards() LIFETIME_BOUND;
    // Of all values, listed in a block or not. A sweep that looks for an operation has nothing to find where its opcode is missing.
    const OpcodeSet& opcodesOfValues() LIFETIME_BOUND;
    // Where there is none, no guard proves an object pristine, and nobody needs guards() to know it.
    bool hasPristineTest() { return opcodesOfValues().contains(Opcode::IsPristine); }
    const InductionVariables& inductionVariables() LIFETIME_BOUND;

    const EffectsOnEdges& effectsOf(ValueID) LIFETIME_BOUND;
    const EffectsOnEdges& effectsOfTerminator(BlockID) LIFETIME_BOUND;
    EffectTraitsOnEdges effectTraitsOfTerminator(BlockID);
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
    bool isGuardAvailable(ValueID guard, ValueID user);
    bool isGuardAvailable(ValueID guard, ProgramPoint point) { return guards().stateAt(guard, point) == GuardState::Available; }
    // Of whoever runs passes on the function, for all who type single values of it. Null where nobody keeps one.
    TypingMemo* typingMemo() const { return m_typingMemo; }
    void setTypingMemo(TypingMemo* memo) { m_typingMemo = memo; }

    EarlierTest findEarlierTest(const Value& test, BlockID at);
    bool hasFailedTestOf(ValueID);

    ExecutionCount executionCountOf(BlockID);
    bool isInCycle(BlockID block) { return cfg().hasCycle() && blockCycles().isInCycle(block); }

    ValueID identityOf(ValueID);
    ValueID sameObjectThroughCasts(ValueID id) const { return Typed::sameObjectThroughCasts(m_function, id); }
    ValueID definitionThroughUpcasts(ValueID id) const { return Typed::definitionThroughUpcasts(m_function, id); }

    bool dominates(ValueID definition, ProgramPoint);
    bool dominates(ValueID definition, const Use&);
    bool dominates(Edge, BlockID);

    template<typename Problem> Vector<std::optional<typename Problem::State>> solveForward(Problem&);

    void invalidate(AnalysisSet);
    AnalysisSet computed() const;
    void didChangeValues() { invalidate(valueAnalyses); }
    void didChangeTypes() { invalidate(valueAnalyses - analysesThatReadNoTypes); }
    // As didChangeTypes(), from whoever has changed types and expected types alone, and no payload.
    void didRetypeValues();
    void didChangeControlFlow() { invalidate(allAnalyses); }

    // Edits that say what they have touched. An analysis that its writer brings up to date in the body of the member stays. Every other one is dropped, as by didChangeValues(),
    // and in the last two as by didChangeControlFlow().
    // The function has one value more, which stands in no list.
    void didAddDetachedValue(ValueID);
    // Values that stood in no list stand in the list of the block.
    void didListValues(BlockID, std::span<const ValueID>);
    // The values have left the list of the block. Until the pass is through with its other blocks, a value that is still listed there may use one of them.
    void didEraseValues(BlockID, std::span<const ValueID> erased);
    // The parameters of the block, its subject, the arguments for its handler, its list of values, an operand of one of those or of its terminator, or what its terminator passes
    // along an edge. No edge was added, removed or redirected.
    void didChangeBlock(BlockID);
    // Every use of the one value names the other.
    void didReplaceUses(ValueID from, ValueID to);
    // Types, payloads or flags. Nothing that didChangeBlock() lists.
    void didChangeValuesButNotUses();
    // Blocks or edges were added, removed or redirected. The blocks are those that were there before and of which something changed that didChangeBlock() lists.
    void didChangeControlFlow(std::span<const BlockID> changedBlocks);
    // compactFunction() has moved blocks and values, and has changed nothing else of what it kept.
    void didRenumber(const Renumbering&);

    void verifyCached();

    void setRunningPass(PassID pass)
    {
        m_runningPass = pass;
        if (m_values.useLists)
            m_values.useLists->endSilence();
        if (m_values.identities && !m_values.identities->isComplete())
            completeIdentitiesBeforePass();
    }
    // Behind a run that has changed the function, where the row of the pass names the use lists, whether the pass has kept them or not.
    void didRunPass()
    {
        if (!m_values.useLists)
            return;
        m_values.useLists->didOutliveDrop();
        m_values.didDropBesideUseLists = true;
    }
    static void logBuilds();

    std::span<const Edge> edgesFromUnreachableBlocks() LIFETIME_BOUND;
    bool isEnteredFromUnreachableBlock(BlockID);
    bool hasSingleIncomingEdge(BlockID);

private:
    friend class EscapeAnalysis;

    static Vector<Edge> collectEdgesFromUnreachableBlocks(const Function&, const CFG&);

    EffectsCache& effectsCache();
    EffectTraitsOnEdges effectTraitsFromCache(ValueID);

    template<bool (EffectTraits::*question)() const, EffectTraits EffectTraitsOnEdges::*edge>
    bool answerFromTraits(ValueID id)
    {
        const EffectTraitBounds& bounds = effectFactsOf(m_function.at(id).opcode).traits;
        bool answer = (bounds.atLeast.*question)();
        if (answer == (bounds.atMost.*question)())
            return answer;
        return (effectTraitsFromCache(id).*edge.*question)();
    }
    // The solve may come long behind escape().
    EscapeAnalysisInputs inputsOfEscapeAnalysis();
    const FailedTests& failedTests();

    struct ControlFlowResults {
        size_t numberOfBlocks { 0 };
        bool mayDescribeFormerGraph { false };
        std::optional<CFG> cfg;
        std::optional<Vector<Edge>> edgesFromUnreachableBlocks;
        std::optional<Dominators> dominators;
        std::optional<NaturalLoops> naturalLoops;
        std::optional<BlockCycles> blockCycles;
    };

    struct ValueResults {
        size_t numberOfValues { 0 };
        // False from where all of them were dropped to where one is asked for: to drop them again is to do nothing.
        bool mayHoldResults { false };
        std::optional<UseLists> useLists;
        // Somebody has said that the function has changed, and the lists were left as they were. A checking build compares them when they are next asked for.
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
    // What was brought up to date stays, whatever it was made from: its writer answers for what it refers to.
    void invalidate(AnalysisSet requested, AnalysisSet broughtUpToDate);
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

    const AnalysisContext m_context;
    const Function& m_function;
    TypingMemo* m_typingMemo { nullptr };
    ChangeLog m_ownChanges;
    // Null where the function is only read.
    ChangeLog* m_changes { nullptr };
    Function* m_functionWithOwnChanges { nullptr };
    // Only constructed when the context has no summaries.
    std::optional<EffectSummaries> m_unknownSummaries;
    const EffectContext m_effectContext;
    // It outlives a change of the function: see EffectsCache::didChangeFunction().
    EffectsCache m_effects;
    ControlFlowResults m_controlFlow;
    ValueResults m_values;
    const bool m_countsBuilds;
    const bool m_verifiesUpdates;
    PassID m_runningPass { PassID::None };
    bool m_everyChangeIsReported { false };
    std::array<PassID, numberOfFunctionAnalyses> m_passesThatDropped { };
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
