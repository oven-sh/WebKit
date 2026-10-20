/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTValueFlowAnalysis.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTValueFlowRules.h"
#include <wtf/MonotonicTime.h>

namespace JSC { namespace AOT { namespace Typed {

static ValueFlowBodies reduceBodiesTimed(const Program& program, BuildContext& build, Seconds& time)
{
    MonotonicTime start = MonotonicTime::now();
    ValueFlowBodies bodies = reduceBodies(program, build);
    time = MonotonicTime::now() - start;
    return bodies;
}

ValueFlowSolution::ValueFlowSolution(const Program& program, BuildContext& build, ValueFlowSolver::Rounds rounds, Witnesses witnesses)
    : m_program(program)
    , m_bodies(reduceBodiesTimed(program, build, m_timeToReduceBodies))
    , m_maker(m_bodies.sets())
{
    m_input = {
        .program = &program,
        .universe = &m_bodies.universe(),
        .nodes = &m_bodies.nodes(),
        .escaped = ValueFlowProgramNodes::escaped(),
        .unknownObject = ValueFlowUniverse::unknownObject(),
        .objectsWithLayoutOfProgram = m_bodies.universe().standIn().covered,
        .numberOfNodes = m_bodies.nodes().numberOfNodes(),
        .edges = m_bodies.edges(),
        .masks = m_bodies.masks(),
        .statements = m_bodies.statements(),
        .operands = m_bodies.operands(),
        .edgesOnceReached = m_bodies.edgesOnceReached(),
        .seedsOnceReached = m_bodies.seedsOnceReached(),
        .functionsEnteredOnceReached = m_bodies.functionsEnteredOnceReached(),
        .ranges = m_bodies.ranges(),
        .mostEdgesLogged = build.limit(LimitID::MaxValueFlowEdgesLogged),
    };
    m_input.remembersWhoLoggedEachEdge = witnesses == Witnesses::ForEveryEdge;
    MonotonicTime start = MonotonicTime::now();
    m_solver = makeUnique<ValueFlowSolver>(m_input, m_bodies.sets(), runValueFlowRule, build.parallelLoops(), rounds);
    m_hasResult = m_solver->solve();
    if (m_hasResult)
        gatherCopies();
    m_timeToSolve = MonotonicTime::now() - start;
}

// One piece of code runs for a function's body and for all copies of it, so what a position of the function holds is what it holds in any of them.
void ValueFlowSolution::gatherCopies()
{
    for (const ValueFlowBodyCopy& copy : m_bodies.copies()) {
        uint32_t firstNode = m_bodies.nodes().firstNodeOf(copy.function);
        for (uint32_t index = 0; index < copy.numberOfNodes; ++index) {
            ValueFlowNodeID node(firstNode + index);
            m_solver->setSolvedValue(node, m_maker.unite(m_solver->valueOf(node), m_solver->valueOf(ValueFlowNodeID(copy.firstNode.index() + index))));
        }
    }
}

ValueFlowSolution::~ValueFlowSolution() = default;

ValueSetID ValueFlowSolution::valueOf(const ValueFlowOperand& operand)
{
    ValueSetID value = operand.constant ? operand.constant : ValueSets::nothing;
    if (operand.node)
        value = m_maker.unite(value, m_solver->valueOf(operand.node));
    return m_maker.intersect(value, operand.mask ? operand.mask : ValueSets::anything);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
