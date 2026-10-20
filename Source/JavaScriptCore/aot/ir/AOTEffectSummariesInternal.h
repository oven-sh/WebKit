/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTEffectSummaries.h"
#include "AOTStronglyConnectedComponents.h"
#include <span>
#include <wtf/BitVector.h>
#include <wtf/FastMalloc.h>
#include <wtf/MallocSpan.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class PhaseTimes;

namespace Typed {

enum class SummaryPart : uint8_t { OnReturn, OnThrow, Parameter, UnknownCode };

struct SummaryNode {
    FunctionID function;
    uint32_t parameter : 30 { 0 };
    SummaryPart part : 2 { SummaryPart::UnknownCode };

    static SummaryNode onReturn(FunctionID function) { return { function, 0, SummaryPart::OnReturn }; }
    static SummaryNode onThrow(FunctionID function) { return { function, 0, SummaryPart::OnThrow }; }
    static SummaryNode parameterOf(FunctionID function, uint32_t parameter)
    {
        RELEASE_ASSERT(parameter < 1u << 30);
        return { function, parameter, SummaryPart::Parameter };
    }
    static SummaryNode unknownCode() { return { }; }

    friend bool operator==(const SummaryNode&, const SummaryNode&) = default;
};
static_assert(sizeof(SummaryNode) == 8);

struct SummaryDependency {
    SummaryNode source;
    SummaryNode target;

    friend bool operator==(const SummaryDependency&, const SummaryDependency&) = default;
};

struct BodySummary {
    Effects onReturn { Effects::none() };
    Effects onThrow { Effects::none() };
    Vector<ParameterSummary> parameters;
    Vector<SummaryDependency> dependencies;
    UnknownCodeWitness unknownCodeWitness;

    friend bool operator==(const BodySummary&, const BodySummary&) = default;
};

// What each body does by itself, kept from one computation of the summaries to the next. The summary of a body reads the body, the records of the functions
// that its dependencies name, and records that all functions share. Whoever keeps this says which functions changed, and drops it when a shared record did.
class BodySummaries {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(BodySummaries);
    WTF_MAKE_NONCOPYABLE(BodySummaries);
public:
    BodySummaries() = default;

    void didChange(std::span<const FunctionID>);

private:
    friend class EffectSummarizer;

    ParallelArray<BodySummary> m_bodies;
    Vector<uint8_t> m_isChanged;
};

struct ExitReachability {
    BitVector reachesReturn;
    BitVector reachesThrow;
};

// What the role of a function says it does, whatever its body looks like. False for a role that says nothing.
bool describeByRole(const Program&, const Function&, EffectDescription&);
// The rows of a serializer or a parser say more than the built-in they stand for: they name no operand types, and they write what the function has just allocated.
// Its summary is the one of the built-in before and after the summarizer sees the body, so a caller that has placed its tests by one of them is right by the other.
constexpr bool isSummarizedByRole(FunctionRole role) { return role == FunctionRole::Serializer || role == FunctionRole::Parser || role == FunctionRole::ValidatingParser; }

ExitReachability computeExitReachability(const Function&);
BodySummary summarizeBodyLocally(const EffectContext&, FunctionID, FunctionAnalyses&);
FunctionSummary createFunctionSummary(Effects onReturn, Effects onThrow, Vector<ParameterSummary>&&, UnknownCodeWitness);

class SummaryNodeNumbering {
public:
    SummaryNodeNumbering(const Program&, const ParallelLoops&);

    unsigned numberOfNodes() const { return m_numberOfNodes; }
    unsigned indexOf(const SummaryNode&) const;
    unsigned numberOfParametersOf(FunctionID) const;

private:
    Vector<uint32_t> m_firstNode;
    unsigned m_numberOfNodes { 0 };
};

class EffectSummarizer {
public:
    EffectSummarizer(const Program&, const FlowResults&, BuildContext&, BodySummaries* kept);

    EffectSummaries run();

private:
    void summarizeBodies(const EffectSummaries& seed);
    Vector<uint8_t> findBodiesThatStillHold(const ParallelArray<BodySummary>&) const;
    void createNodes();
    void propagateEffects(PhaseTimes&);
    void takeInSources(uint32_t node);
    Vector<uint32_t> resolveNodesBelowCycles();
    void resolveNodesInAndAboveCycles(std::span<const uint32_t> nodes);
    void addEffectsOfUnknownCode();
    void checkEffectsAgainstPlainSolve(std::span<const Effects> effectsOfNodesAlone) const;
    void propagateWitnesses();
    EffectSummaries assemble();

    Effects effectsOfUnknownCode() const;
    std::span<const uint32_t> sourcesOf(unsigned node) const LIFETIME_BOUND;

    const Program& m_program;
    const FlowResults& m_flow;
    BuildContext& m_build;
    BodySummaries* m_kept;
    SummaryNodeNumbering m_numbering;
    ParallelArray<BodySummary> m_bodies;
    MallocSpan<Effects> m_effects;
    Vector<UnknownCodeWitness> m_witnesses;
    Vector<uint32_t> m_firstSource;
    Vector<uint32_t> m_endOfSources;
    Vector<uint32_t> m_sources;
    Vector<uint8_t> m_takesInUnknownCode;
    Vector<uint8_t> m_isResolved;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
