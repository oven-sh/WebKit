/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTValueFlowBodies.h"
#include "AOTValueFlowSolver.h"
#include "AOTValueSets.h"
#include <memory>
#include <wtf/Noncopyable.h>
#include <wtf/Seconds.h>

namespace JSC { namespace AOT { namespace Typed {

// The solved program: what every node holds, which functions are reached, the marks and the uses.
class ValueFlowSolution {
    WTF_MAKE_NONCOPYABLE(ValueFlowSolution);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ValueFlowSolution);
public:
    enum class Witnesses : uint8_t { None, ForEveryEdge };
    ValueFlowSolution(const Program&, BuildContext&, ValueFlowSolver::Rounds = ValueFlowSolver::Rounds::Due, Witnesses = Witnesses::None);
    ~ValueFlowSolution();

    const Program& program() const { return m_program; }
    const ValueSets& sets() const { return m_bodies.sets(); }
    const ValueFlowBodies& bodies() const { return m_bodies; }
    const ValueFlowSolver& solver() const { return *m_solver; }
    // False if the solve has stopped at a limit: then nothing that the solver holds is a result.
    bool hasResult() const { return m_hasResult; }
    ValueSetID valueOf(const ValueFlowOperand&);
    Seconds timeToReduceBodies() const { return m_timeToReduceBodies; }
    Seconds timeToSolve() const { return m_timeToSolve; }

private:
    void gatherCopies();

    const Program& m_program;
    ValueFlowBodies m_bodies;
    ValueSetMaker m_maker;
    ValueFlowSolverInput m_input;
    std::unique_ptr<ValueFlowSolver> m_solver;
    bool m_hasResult { false };
    Seconds m_timeToReduceBodies;
    Seconds m_timeToSolve;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
