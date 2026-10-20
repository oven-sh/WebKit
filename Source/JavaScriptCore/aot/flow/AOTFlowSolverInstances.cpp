/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowSolver.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTFlowCallOperations.h"
#include "AOTFlowFixpointCheck.h"
#include "AOTFlowObjectOperations.h"
#include "AOTFlowPropertyOperations.h"
#include "AOTLimits.h"
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(defaultLimitValue(LimitID::MaxFlowRangeGrowth, Architecture::ARM64) < UINT16_MAX);
static_assert(defaultLimitValue(LimitID::MaxFlowRangeGrowth, Architecture::X86_64) < UINT16_MAX);

static FlowValue filterValue(FlowValueTable& values, FlowFilterKind kind, const FlowValue& value, const Type& type)
{
    switch (kind) {
    case FlowFilterKind::Inside:
        return values.filter(value, type);
    case FlowFilterKind::Outside:
        return values.filterOut(value, type);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

FlowInstanceID FlowSolver::enter(FunctionID function, FlowContext context)
{
    if (!m_graph.summaryOf(function).hasBody())
        return { };
    if (!m_graph.isHost(function) && !m_build.isEnabled(FlowResultKind::CallVersions, function.index()))
        context = FlowContext::Generic;
    if (FlowInstanceID existing = m_graph.findInstance(function, context))
        return existing;

    FlowInstanceID instance = m_graph.createInstance(function, context);
    m_graph.setObjectsOf(instance, createObjectsOfInstance(*this, instance));
    m_worklist.instancesToInitialize.append(instance);
    return instance;
}

FlowInstanceID FlowSolver::enterAtCallSite(FunctionID function, const FlowCallSiteKey& key)
{
    if (!m_graph.summaryOf(function).hasBody())
        return { };
    if (FlowInstanceID existing = m_graph.findCallSiteInstance(function, key))
        return existing;

    Subject subject;
    if (!m_graph.isHost(key.caller)) {
        subject.function = key.caller;
        subject.value = key.call;
    }
    if (m_build.exceeds(LimitID::MaxFlowInstances, m_graph.numberOfCallSiteInstances() + 1, subject))
        return { };

    FlowInstanceID instance = m_graph.createCallSiteInstance(function, key);
    m_graph.setObjectsOf(instance, createObjectsOfInstance(*this, instance));
    m_worklist.instancesToInitialize.append(instance);
    return instance;
}

void FlowSolver::initializeInstance(FlowInstanceID instance)
{
    const FlowSummary& summary = m_graph.summaryOf(instance);
    if (m_fixpointCheck) [[unlikely]]
        m_fixpointCheck->willInitialize(instance);

    m_graph.setState(instance, FlowInstanceState::Initialized);

    for (const FlowSeed& seed : summary.seeds)
        join(node(instance, seed.node()), valueOfSeed(instance, seed), witnessAt(instance, summary.originOf(seed.node())));

    Witness witness = witnessAt(instance, ValueID());
    for (auto& [from, to] : summary.copiesFromExternals)
        connect(node(instance, from), node(instance, to), { }, witness);

    if (isPastWorkLimit(summary.function)) {
        abandon(instance);
        return;
    }

    summary.statements.forEach([&](FlowStatementReference statement) {
        run(FlowStatementSite { instance, statement });
    });
}

FlowValue FlowSolver::valueOfSeed(FlowInstanceID instance, const FlowSeed& seed)
{
    switch (seed.kind()) {
    case FlowSeedKind::Primitives:
        return values().valueOfPrimitives(seed.primitives());
    case FlowSeedKind::Integers:
        return values().valueOfIntegers(m_graph.summaryOf(instance).ranges[seed.rangeIndex()]);
    case FlowSeedKind::String:
        return values().valueOfString(seed.string());
    case FlowSeedKind::Symbol:
        return values().valueOfSymbol(seed.symbol());
    case FlowSeedKind::Site:
        return values().valueOfObject(m_graph.objectOf(instance, seed.site()));
    case FlowSeedKind::Builtin:
        RELEASE_ASSERT(isObjectKind(builtins().at(seed.builtin()).kind));
        return values().valueOfObject(ensureBuiltinObject(*this, seed.builtin()));
    case FlowSeedKind::ModuleBuiltin:
        return values().valueOfObject(ensureModuleBuiltinObject(*this, seed.builtin(), seed.module()));
    case FlowSeedKind::Namespace:
        return values().valueOfObject(ensureNamespaceObject(*this, seed.module()));
    case FlowSeedKind::TypeAsRead:
        return values().valueOfType(seed.type());
    case FlowSeedKind::Unknown:
        return values().unknownValue();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void FlowSolver::run(FlowStatementSite site)
{
    ++m_runNumber;
    if (m_graph.at(site.instance).state == FlowInstanceState::PastWorkLimit)
        return;
    charge(site, 1);
    if (m_fixpointCheck) [[unlikely]]
        m_fixpointCheck->willRun(site);

    RELEASE_ASSERT(!m_memberInProgress.cursor);
    uint64_t changesBefore = numberOfChanges();
    runStatement(site);

    size_t kind = static_cast<size_t>(site.statement.kind);
    ++m_counters.statementRuns;
    ++m_counters.runsOfKind[kind];
    if (numberOfChanges() != changesBefore)
        return;
    ++m_counters.statementRunsWithoutChange;
    ++m_counters.runsWithoutChangeOfKind[kind];
}

void FlowSolver::runStatement(FlowStatementSite site)
{
    const FlowStatements& statements = summaryOf(site).statements;
    uint32_t index = site.statement.index;

    switch (site.statement.kind) {
    case FlowStatementKind::Filter:
        runFilter(site, statements.filters[index]);
        return;
    case FlowStatementKind::ConditionalSeed:
        runConditionalSeed(site, statements.conditionalSeeds[index]);
        return;
    case FlowStatementKind::NaturalOperation:
        runNaturalOperation(site, statements.naturalOperations[index]);
        return;
    case FlowStatementKind::Allocation:
        runAllocation(*this, site, statements.allocations[index]);
        return;
    case FlowStatementKind::LocationAccess:
        runLocationAccess(*this, site, statements.locationAccesses[index]);
        return;
    case FlowStatementKind::FieldAccess:
        runFieldAccess(*this, site, statements.fieldAccesses[index]);
        return;
    case FlowStatementKind::PropertyAccess:
        runPropertyAccess(*this, site, statements.propertyAccesses[index]);
        return;
    case FlowStatementKind::ElementAccess:
        runElementAccess(*this, site, statements.elementAccesses[index]);
        return;
    case FlowStatementKind::SlotAccess:
        runSlotAccess(*this, site, statements.slotAccesses[index]);
        return;
    case FlowStatementKind::PrototypeAccess:
        runPrototypeAccess(*this, site, statements.prototypeAccesses[index]);
        return;
    case FlowStatementKind::PropertyCopy:
        runPropertyCopy(*this, site, statements.propertyCopies[index]);
        return;
    case FlowStatementKind::Call:
        runCall(*this, site, statements.calls[index]);
        return;
    case FlowStatementKind::DirectCall:
        runDirectCall(*this, site, statements.directCalls[index]);
        return;
    case FlowStatementKind::ClassDefinition:
        runClassDefinition(*this, site, statements.classDefinitions[index]);
        return;
    case FlowStatementKind::MethodDefinition:
        runMethodDefinition(*this, site, statements.methodDefinitions[index]);
        return;
    case FlowStatementKind::Await:
        runAwait(*this, site, statements.awaits[index]);
        return;
    case FlowStatementKind::Iteration:
        runIteration(*this, site, statements.iterations[index]);
        return;
    case FlowStatementKind::OperandUse:
        runOperandUse(*this, site, statements.operandUses[index]);
        return;
    case FlowStatementKind::ModuleOperation:
        runModuleOperation(*this, site, statements.moduleOperations[index]);
        return;
    case FlowStatementKind::UnseenCodeEvent:
        runUnseenCodeEvent(*this, site, statements.unseenCodeEvents[index]);
        return;
    case FlowStatementKind::BuiltinCall:
        runBuiltinCall(*this, site);
        return;
    case FlowStatementKind::Lookup:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void FlowSolver::runFilter(FlowStatementSite site, const FlowFilter& filter)
{
    if (filter.field) {
        runFieldFilter(*this, site, filter);
        return;
    }

    Witness witness = witnessAt(site, ValueID());
    switch (filter.kind) {
    case FlowFilterKind::Inside:
        connect(node(site, filter.from), node(site, filter.to), filter.type, witness);
        return;
    case FlowFilterKind::Outside: {
        FlowNodeID from = node(site, filter.from);
        FlowValue arrived = valueOf(from).withoutObjects();
        arrived.objects = takeNewObjects(site, 0, from);
        join(node(site, filter.to), values().filterOut(arrived, m_program.at(filter.type)), witness);
        return;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void FlowSolver::runConditionalSeed(FlowStatementSite site, const FlowConditionalSeed& statement)
{
    if (filterValue(values(), statement.kind, valueAt(site, statement.from), m_program.at(statement.type)).isEmpty())
        return;
    join(node(site, statement.seed.node()), valueOfSeed(site.instance, statement.seed), witnessAt(site, statement.origin));
}

void FlowSolver::runNaturalOperation(FlowStatementSite site, const FlowNaturalOperation& statement)
{
    if (!statement.result)
        return;

    const Value& operation = operationAt(site, statement.origin);
    Vector<FlowValue, FlowNaturalEvaluator::maxOperandCount> operands;
    for (FlowLocalID local : summaryOf(site).statements.list(statement.operands))
        operands.append(valueAt(site, local));

    FlowNodeID target = node(site, statement.result);
    // A narrowed operand is part of the operand, so no range is made here that could grow for ever.
    if (statement.kind != FlowNaturalKind::Result) {
        join(target, m_evaluator.narrowOperand(operation, statement.narrowedOperand, statement.kind == FlowNaturalKind::OperandOnTakenEdge, operands.span()));
        return;
    }
    join(target, widenGrowingRange(site, valueOf(target), m_evaluator.evaluate(operation, operands.span())));
}

FlowValue FlowSolver::widenGrowingRange(FlowStatementSite site, const FlowValue& previous, FlowValue&& result)
{
    if (!result.integers || !previous.integers)
        return WTF::move(result);

    Range previousRange = values().rangeOf(previous.integers);
    Range resultRange = values().rangeOf(result.integers);
    if (resultRange.isSubsetOf(previousRange))
        return WTF::move(result);

    uint64_t maxGrowth = m_build.limit(LimitID::MaxFlowRangeGrowth);
    uint16_t& growth = m_graph.rangeGrowth(site.instance, site.statement.index);
    if (growth < maxGrowth) {
        ++growth;
        return WTF::move(result);
    }
    if (growth == maxGrowth) {
        ++growth;
        m_build.gaveUp(LimitID::MaxFlowRangeGrowth, Subject {
            .function = m_graph.at(site.instance).function,
            .value = summaryOf(site).statements.naturalOperations[site.statement.index].origin,
        });
    }

    Range widenedRange = previousRange.unionWith(resultRange).widenedFrom(previousRange);
    result.integers = values().valueOfIntegers(widenedRange).integers;
    return WTF::move(result);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
