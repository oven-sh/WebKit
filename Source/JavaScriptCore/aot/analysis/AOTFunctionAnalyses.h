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
#include "AOTDiagnostics.h"
#include "AOTDominators.h"
#include "AOTEffectSummaries.h"
#include "AOTEffectsCache.h"
#include "AOTEscapeAnalysis.h"
#include "AOTGuardAvailability.h"
#include "AOTInductionVariables.h"
#include "AOTLiveness.h"
#include "AOTNaturalLoops.h"
#include "AOTUseEffectHistory.h"
#include "AOTUseLists.h"
#include "AOTValueIdentity.h"
#include <optional>
#include <span>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

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

inline constexpr AnalysisSet allAnalyses = valueAnalyses | AnalysisSet {
    FunctionAnalysis::ControlFlow,
    FunctionAnalysis::Dominators,
    FunctionAnalysis::NaturalLoops,
    FunctionAnalysis::Cycles,
};

class FunctionAnalyses {
    WTF_MAKE_NONCOPYABLE(FunctionAnalyses);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(FunctionAnalyses);
public:
    FunctionAnalyses(const AnalysisContext&, const Function&);
    ~FunctionAnalyses();

    const AnalysisContext& context() const LIFETIME_BOUND { return m_context; }
    const EffectContext& effectContext() const LIFETIME_BOUND { return m_effectContext; }
    const Program& program() const { return m_context.program; }
    const Function& function() const { return m_function; }
    Subject subject(ValueID = { }) const;

    CFG& cfg() LIFETIME_BOUND;
    Dominators& dominators() LIFETIME_BOUND;
    NaturalLoops& naturalLoops() LIFETIME_BOUND;
    const BlockCycles& blockCycles() LIFETIME_BOUND;
    const UseLists& useLists() LIFETIME_BOUND;
    const ValueIdentities& identities() LIFETIME_BOUND;
    const Liveness& liveness() LIFETIME_BOUND;
    const EscapeAnalysis& escape() LIFETIME_BOUND;
    const UseEffectHistory& escapeHistory() LIFETIME_BOUND;
    const BlockFrequencies& blockFrequencies() LIFETIME_BOUND;
    const GuardAvailability& guards() LIFETIME_BOUND;
    // Where there is none, no guard proves an object pristine, and nobody needs guards() to know it.
    bool hasPristineTest();
    const InductionVariables& inductionVariables() LIFETIME_BOUND;

    const EffectsOnEdges& effectsOf(ValueID) LIFETIME_BOUND;
    const EffectsOnEdges& effectsOfTerminator(BlockID) LIFETIME_BOUND;
    bool mayThrow(ValueID id) { return effectsOf(id).mayThrow(); }

    const GuardFacts& guardFacts(ValueID guard) LIFETIME_BOUND { return guards().factsOf(guard); }
    GuardState guardStateAt(ValueID guard, const Use&);
    bool isGuardAvailable(ValueID guard, ValueID user);
    bool isGuardAvailable(ValueID guard, ProgramPoint point) { return guards().stateAt(guard, point) == GuardState::Available; }
    EarlierTest findEarlierTest(const Value& test, BlockID at);

    ExecutionCount executionCountOf(BlockID);

    ValueID identityOf(ValueID id) { return identities().identityOf(id); }
    ValueID sameObjectThroughCasts(ValueID id) const { return Typed::sameObjectThroughCasts(m_function, id); }
    ValueID definitionThroughUpcasts(ValueID id) const { return Typed::definitionThroughUpcasts(m_function, id); }

    bool dominates(ValueID definition, ProgramPoint);
    bool dominates(ValueID definition, const Use&);
    bool dominates(Edge, BlockID);

    template<typename Problem> Vector<std::optional<typename Problem::State>> solveForward(Problem&);

    void invalidate(AnalysisSet);
    AnalysisSet computed() const;
    void didChangeValues() { invalidate(valueAnalyses); }
    void didChangeControlFlow() { invalidate(allAnalyses); }

    void verifyCached() const;

    std::span<const Edge> edgesFromUnreachableBlocks() LIFETIME_BOUND;
    bool isEnteredFromUnreachableBlock(BlockID);
    bool hasSingleIncomingEdge(BlockID);

private:
    static Vector<Edge> collectEdgesFromUnreachableBlocks(const Function&, const CFG&);

    EffectsCache& effectsCache();
    const FailedTests& failedTests();

    struct ControlFlowResults {
        size_t numberOfBlocks { 0 };
        std::optional<CFG> cfg;
        std::optional<Vector<Edge>> edgesFromUnreachableBlocks;
        std::optional<Dominators> dominators;
        std::optional<NaturalLoops> naturalLoops;
        std::optional<BlockCycles> blockCycles;
    };

    struct ValueResults {
        size_t numberOfValues { 0 };
        std::optional<UseLists> useLists;
        std::optional<ValueIdentities> identities;
        std::optional<Liveness> liveness;
        std::optional<EffectsCache> effects;
        std::optional<EscapeAnalysis> escape;
        std::optional<UseEffectHistory> escapeHistory;
        std::optional<BlockFrequencies> blockFrequencies;
        std::optional<GuardAvailability> guards;
        std::optional<bool> hasPristineTest;
        size_t numberOfValuesSearchedForPristineTest { 0 };
        std::optional<FailedTests> failedTests;
        std::optional<InductionVariables> inductionVariables;
    };

    void checkBlockCount() const { RELEASE_ASSERT(m_function.blocks.size() == m_controlFlow.numberOfBlocks); }
    void checkValueCount() const { RELEASE_ASSERT(m_function.values.size() == m_values.numberOfValues); }
    void willComputeValueAnalysis();

    const AnalysisContext m_context;
    const Function& m_function;
    const EffectSummaries m_unknownSummaries;
    const EffectContext m_effectContext;
    ControlFlowResults m_controlFlow;
    ValueResults m_values;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
