/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFunctionAnalyses.h"

#if ENABLE(AOT)

#include "AOTFlowResults.h"
#include <algorithm>

namespace JSC { namespace AOT { namespace Typed {

static AnalysisSet addDependentAnalyses(AnalysisSet set)
{
    if (set.contains(FunctionAnalysis::ControlFlow))
        set.add(allAnalyses);
    if (set.contains(FunctionAnalysis::Dominators))
        set.add({ FunctionAnalysis::NaturalLoops, FunctionAnalysis::Guards, FunctionAnalysis::FailedTests, FunctionAnalysis::Induction });
    if (set.contains(FunctionAnalysis::NaturalLoops))
        set.add(FunctionAnalysis::Induction);
    if (set.contains(FunctionAnalysis::Cycles))
        set.add({ FunctionAnalysis::Escape, FunctionAnalysis::Frequencies });
    if (set.contains(FunctionAnalysis::UseLists))
        set.add({ FunctionAnalysis::Liveness, FunctionAnalysis::Escape, FunctionAnalysis::Guards, FunctionAnalysis::Induction });
    if (set.contains(FunctionAnalysis::Identities))
        set.add({ FunctionAnalysis::Guards, FunctionAnalysis::FailedTests, FunctionAnalysis::Induction });
    if (set.contains(FunctionAnalysis::Effects))
        set.add({ FunctionAnalysis::Escape, FunctionAnalysis::EscapeHistory, FunctionAnalysis::Guards, FunctionAnalysis::Induction });
    if (set.contains(FunctionAnalysis::Escape))
        set.add({ FunctionAnalysis::EscapeHistory, FunctionAnalysis::Guards });
    if (set.contains(FunctionAnalysis::EscapeHistory))
        set.add(FunctionAnalysis::Guards);
    return set;
}

FunctionAnalyses::FunctionAnalyses(const AnalysisContext& context, const Function& function)
    : m_context(context)
    , m_function(function)
    , m_unknownSummaries()
    , m_effectContext { context.program, context.effects ? *context.effects : m_unknownSummaries }
{
}

FunctionAnalyses::~FunctionAnalyses() = default;

Subject FunctionAnalyses::subject(ValueID value) const
{
    if (!m_context.id)
        return { };

    FunctionID known = m_context.id;
    if (ProvisionalFunctions::contains(known))
        known = m_function.derivation.source;
    while (ProvisionalFunctions::contains(known))
        known = m_context.program.at(known).derivation.source;

    Subject result { .function = findFunctionAsRead(&m_context.program, known), .location = m_function.declarationLocation() };
    if (!value)
        return result;

    SourceLocation location = m_function.locationOf(value);
    if (hasModule(location))
        result.location = location;
    if (result.function == m_context.id)
        result.value = value;
    return result;
}

CFG& FunctionAnalyses::cfg()
{
    if (!m_controlFlow.cfg) {
        m_controlFlow.cfg.emplace(m_function);
        m_controlFlow.numberOfBlocks = m_function.blocks.size();
    }
    checkBlockCount();
    return *m_controlFlow.cfg;
}

Dominators& FunctionAnalyses::dominators()
{
    if (!m_controlFlow.dominators)
        m_controlFlow.dominators.emplace(cfg(), false);
    checkBlockCount();
    return *m_controlFlow.dominators;
}

NaturalLoops& FunctionAnalyses::naturalLoops()
{
    if (!m_controlFlow.naturalLoops)
        m_controlFlow.naturalLoops.emplace(cfg(), dominators(), false);
    checkBlockCount();
    return *m_controlFlow.naturalLoops;
}

const BlockCycles& FunctionAnalyses::blockCycles()
{
    if (!m_controlFlow.blockCycles)
        m_controlFlow.blockCycles.emplace(cfg());
    checkBlockCount();
    return *m_controlFlow.blockCycles;
}

void FunctionAnalyses::willComputeValueAnalysis()
{
    if (!computed().containsAny(valueAnalyses))
        m_values.numberOfValues = m_function.values.size();
}

const UseLists& FunctionAnalyses::useLists()
{
    if (!m_values.useLists) {
        willComputeValueAnalysis();
        m_values.useLists.emplace(m_function, cfg());
    }
    checkBlockCount();
    checkValueCount();
    return *m_values.useLists;
}

const ValueIdentities& FunctionAnalyses::identities()
{
    if (!m_values.identities) {
        willComputeValueAnalysis();
        m_values.identities.emplace(m_function, cfg());
    }
    checkBlockCount();
    checkValueCount();
    return *m_values.identities;
}

const Liveness& FunctionAnalyses::liveness()
{
    if (!m_values.liveness) {
        willComputeValueAnalysis();
        m_values.liveness.emplace(m_function, cfg(), useLists());
    }
    checkBlockCount();
    checkValueCount();
    return *m_values.liveness;
}

EffectsCache& FunctionAnalyses::effectsCache()
{
    if (!m_values.effects) {
        willComputeValueAnalysis();
        m_values.effects.emplace(m_effectContext, m_function);
    }
    RELEASE_ASSERT(m_values.effects->numberOfBlocks() == m_function.blocks.size());
    checkValueCount();
    return *m_values.effects;
}

const EscapeAnalysis& FunctionAnalyses::escape()
{
    if (!m_values.escape) {
        willComputeValueAnalysis();
        m_values.escape.emplace(EscapeAnalysisInputs { m_context, m_effectContext, effectsCache(), m_function, cfg(), useLists(), blockCycles(), subject() });
    }
    checkBlockCount();
    checkValueCount();
    return *m_values.escape;
}

const UseEffectHistory& FunctionAnalyses::escapeHistory()
{
    if (!m_values.escapeHistory) {
        willComputeValueAnalysis();
        escape();
        m_values.escapeHistory.emplace(*this);
    }
    checkBlockCount();
    checkValueCount();
    return *m_values.escapeHistory;
}

const BlockFrequencies& FunctionAnalyses::blockFrequencies()
{
    if (!m_values.blockFrequencies) {
        willComputeValueAnalysis();
        m_values.blockFrequencies.emplace(m_context, m_function, cfg(), blockCycles());
    }
    checkBlockCount();
    checkValueCount();
    return *m_values.blockFrequencies;
}

const GuardAvailability& FunctionAnalyses::guards()
{
    if (!m_values.guards) {
        willComputeValueAnalysis();
        cfg();
        useLists();
        identities();
        m_values.guards.emplace(*this);
    }
    checkBlockCount();
    checkValueCount();
    return *m_values.guards;
}

bool FunctionAnalyses::hasPristineTest()
{
    if (!m_values.hasPristineTest || m_values.numberOfValuesSearchedForPristineTest != m_function.values.size()) {
        m_values.numberOfValuesSearchedForPristineTest = m_function.values.size();
        m_values.hasPristineTest = m_function.values.containsIf([](const Value& value) {
            return value.opcode == Opcode::IsPristine;
        });
    }
    return *m_values.hasPristineTest;
}

const FailedTests& FunctionAnalyses::failedTests()
{
    if (!m_values.failedTests) {
        willComputeValueAnalysis();
        m_values.failedTests.emplace(*this);
    }
    checkBlockCount();
    checkValueCount();
    return *m_values.failedTests;
}

const InductionVariables& FunctionAnalyses::inductionVariables()
{
    if (!m_values.inductionVariables) {
        willComputeValueAnalysis();
        m_values.inductionVariables.emplace(*this);
    }
    checkBlockCount();
    checkValueCount();
    return *m_values.inductionVariables;
}

const EffectsOnEdges& FunctionAnalyses::effectsOf(ValueID id)
{
    return effectsCache().of(id);
}

const EffectsOnEdges& FunctionAnalyses::effectsOfTerminator(BlockID block)
{
    return effectsCache().ofTerminator(block);
}

GuardState FunctionAnalyses::guardStateAt(ValueID guard, const Use& use)
{
    switch (use.kind) {
    case Use::Kind::Operand: {
        const Value& user = m_function.at(use.user);
        if (user.operandKind(use.operandIndex) == OperandKind::Guard)
            return guards().stateAt(guard, user, useLists().pointOf(use));
        return guards().stateAt(guard, useLists().pointOf(use));
    }
    case Use::Kind::TerminatorOperand:
        return guards().stateAt(guard, useLists().pointOf(use));
    case Use::Kind::EdgeArgument:
    case Use::Kind::HandlerArgument:
    case Use::Kind::BlockSubject:
        return GuardState::Available;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool FunctionAnalyses::isGuardAvailable(ValueID guard, ValueID user)
{
    const Value& value = m_function.at(user);
    std::optional<uint32_t> guardOperandIndex;
    value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (!guardOperandIndex && operand == guard && value.operandKind(operandIndex) == OperandKind::Guard)
            guardOperandIndex = operandIndex;
    });
    RELEASE_ASSERT(guardOperandIndex);

    ValueLocation location = useLists().locationOf(user);
    if (location.placement != ValuePlacement::InBlock)
        return false;
    return guardStateAt(guard, Use { Use::Kind::Operand, location.block, user, *guardOperandIndex, 0 }) == GuardState::Available;
}

EarlierTest FunctionAnalyses::findEarlierTest(const Value& test, BlockID at)
{
    return failedTests().find(test, at);
}

ExecutionCount FunctionAnalyses::executionCountOf(BlockID block)
{
    if (!m_context.flow || !m_context.id || !m_context.flow->runsAtMostOnce(m_context.program, m_context.id))
        return ExecutionCount::Unknown;
    if (!cfg().isReachable(block) || blockCycles().isInCycle(block))
        return ExecutionCount::Unknown;
    return ExecutionCount::AtMostOnce;
}

bool FunctionAnalyses::dominates(ValueID definition, ProgramPoint point)
{
    ValueLocation location = useLists().locationOf(definition);
    if (!cfg().isReachable(point.block))
        return false;

    switch (location.placement) {
    case ValuePlacement::Detached:
        return false;
    case ValuePlacement::Parameter:
        return dominators().dominates(location.block, point.block);
    case ValuePlacement::InBlock:
        if (location.block == point.block)
            return location.index < point.index;
        return dominators().terminatorDominates(location.block, point.block);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool FunctionAnalyses::dominates(ValueID definition, const Use& use)
{
    return dominates(definition, useLists().pointOf(use));
}

bool FunctionAnalyses::dominates(Edge edge, BlockID block)
{
    CFG& graph = cfg();
    if (!graph.isReachable(edge.from) || !graph.isReachable(block))
        return false;
    BlockID target = targetOf(m_function, edge).block;
    return graph.singleIncomingEdge(target) == edge && dominators().dominates(target, block);
}

void FunctionAnalyses::invalidate(AnalysisSet requested)
{
    AnalysisSet set = addDependentAnalyses(requested);
    if (set.contains(FunctionAnalysis::Induction))
        m_values.inductionVariables.reset();
    if (set.contains(FunctionAnalysis::FailedTests))
        m_values.failedTests.reset();
    if (set.contains(FunctionAnalysis::Guards)) {
        m_values.guards.reset();
        m_values.hasPristineTest.reset();
    }
    if (set.contains(FunctionAnalysis::Frequencies))
        m_values.blockFrequencies.reset();
    if (set.contains(FunctionAnalysis::EscapeHistory))
        m_values.escapeHistory.reset();
    if (set.contains(FunctionAnalysis::Escape))
        m_values.escape.reset();
    if (set.contains(FunctionAnalysis::Effects))
        m_values.effects.reset();
    if (set.contains(FunctionAnalysis::Liveness))
        m_values.liveness.reset();
    if (set.contains(FunctionAnalysis::Identities))
        m_values.identities.reset();
    if (set.contains(FunctionAnalysis::UseLists))
        m_values.useLists.reset();
    if (set.contains(FunctionAnalysis::NaturalLoops))
        m_controlFlow.naturalLoops.reset();
    if (set.contains(FunctionAnalysis::Dominators))
        m_controlFlow.dominators.reset();
    if (set.contains(FunctionAnalysis::Cycles))
        m_controlFlow.blockCycles.reset();
    if (set.contains(FunctionAnalysis::ControlFlow)) {
        m_controlFlow.edgesFromUnreachableBlocks.reset();
        m_controlFlow.cfg.reset();
    }
}

AnalysisSet FunctionAnalyses::computed() const
{
    AnalysisSet result;
    result.set(FunctionAnalysis::ControlFlow, m_controlFlow.cfg.has_value());
    result.set(FunctionAnalysis::Dominators, m_controlFlow.dominators.has_value());
    result.set(FunctionAnalysis::NaturalLoops, m_controlFlow.naturalLoops.has_value());
    result.set(FunctionAnalysis::Cycles, m_controlFlow.blockCycles.has_value());
    result.set(FunctionAnalysis::UseLists, m_values.useLists.has_value());
    result.set(FunctionAnalysis::Identities, m_values.identities.has_value());
    result.set(FunctionAnalysis::Liveness, m_values.liveness.has_value());
    result.set(FunctionAnalysis::Effects, m_values.effects.has_value());
    result.set(FunctionAnalysis::Escape, m_values.escape.has_value());
    result.set(FunctionAnalysis::EscapeHistory, m_values.escapeHistory.has_value());
    result.set(FunctionAnalysis::Frequencies, m_values.blockFrequencies.has_value());
    result.set(FunctionAnalysis::Guards, m_values.guards.has_value());
    result.set(FunctionAnalysis::FailedTests, m_values.failedTests.has_value());
    result.set(FunctionAnalysis::Induction, m_values.inductionVariables.has_value());
    return result;
}

std::span<const Edge> FunctionAnalyses::edgesFromUnreachableBlocks()
{
    if (!m_controlFlow.edgesFromUnreachableBlocks)
        m_controlFlow.edgesFromUnreachableBlocks.emplace(collectEdgesFromUnreachableBlocks(m_function, cfg()));
    checkBlockCount();
    return m_controlFlow.edgesFromUnreachableBlocks->span();
}

bool FunctionAnalyses::isEnteredFromUnreachableBlock(BlockID block)
{
    return std::ranges::any_of(edgesFromUnreachableBlocks(), [&](Edge edge) {
        return targetOf(m_function, edge).block == block;
    });
}

bool FunctionAnalyses::hasSingleIncomingEdge(BlockID block)
{
    return cfg().singleIncomingEdge(block) && !isEnteredFromUnreachableBlock(block);
}

Vector<Edge> FunctionAnalyses::collectEdgesFromUnreachableBlocks(const Function& function, const CFG& graph)
{
    Vector<Edge> edges;
    for (unsigned blockIndex = 0; blockIndex < graph.numNodes(); ++blockIndex) {
        BlockID block(blockIndex);
        if (graph.isReachable(block))
            continue;
        forEachOutgoingEdge(function, block, [&](Edge edge, const Target&) {
            edges.append(edge);
        });
    }
    return edges;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
