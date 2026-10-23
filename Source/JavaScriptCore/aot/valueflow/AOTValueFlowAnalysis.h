/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTValueFlowBodies.h"
#include "AOTValueFlowSolver.h"
#include "AOTValueSets.h"
#include "AOTVector.h"
#include <memory>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/Seconds.h>

namespace JSC { namespace AOT { namespace Typed {

class ValueSetMakerPool {
    WTF_MAKE_NONCOPYABLE(ValueSetMakerPool);
public:
    explicit ValueSetMakerPool(ValueSets& sets)
        : m_sets(sets)
    {
    }

    ValueSetMaker& take();
    void giveBack(ValueSetMaker&);

private:
    ValueSets& m_sets;
    Lock m_lock;
    Vector<ValueSetMaker*> m_idleMakers WTF_GUARDED_BY_LOCK(m_lock);
    Vector<std::unique_ptr<ValueSetMaker>> m_ownMakers WTF_GUARDED_BY_LOCK(m_lock);
};

class BorrowedValueSetMaker {
    WTF_MAKE_NONCOPYABLE(BorrowedValueSetMaker);
public:
    explicit BorrowedValueSetMaker(ValueSetMakerPool& pool)
        : m_pool(pool)
        , m_maker(pool.take())
    {
    }

    ~BorrowedValueSetMaker() { m_pool.giveBack(m_maker); }

    ValueSetMaker& get() { return m_maker; }

private:
    ValueSetMakerPool& m_pool;
    ValueSetMaker& m_maker;
};

// The solved program: what every node holds, which functions are reached, the marks and the uses.
class ValueFlowSolution {
    WTF_MAKE_NONCOPYABLE(ValueFlowSolution);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ValueFlowSolution);
public:
    // A build that collects the report on sites remembers who logged each edge whatever is asked for here.
    enum class Witnesses : uint8_t { None, ForEveryEdge };
    // Whether anybody asks the solver how a value has come to a node: see ValueFlowSolverInput::isAskedHowValuesCame.
    enum class WaysOfValues : uint8_t { AreAskedFor, AreNotAskedFor };
    // It makes property keys of the texts that calls pass where a function takes a key: see ValueFlowInstancedFunctions.
    ValueFlowSolution(Program&, BuildContext&, ValueFlowSolver::Rounds = ValueFlowSolver::Rounds::Due, Witnesses = Witnesses::None, uint32_t mostRounds = UINT32_MAX, EnumSet<ValueFlowUnknownSource> unknownSourcesLeftOut = { }, WaysOfValues = WaysOfValues::AreAskedFor);
    ~ValueFlowSolution();

    const Program& program() const { return m_program; }
    const ValueSets& sets() const { return m_bodies.sets(); }
    const ValueFlowBodies& bodies() const { return m_bodies; }
    const ValueFlowSolver& solver() const { return *m_solver; }
    ValueSetMakerPool& makerPool() const { return m_makerPool; }
    // A solve of a program always ends with a result: past its limits it gives sets up, and the untracked object stands for what was in them.
    bool hasResult() const { return m_hasResult; }
    // Through the mask of the operand, as a rule sees it, but not in the normal form: beside a placeholder the set may or may not list what the placeholder covers.
    ValueSetID valueOf(const ValueFlowOperand&);
    Seconds timeToReduceBodies() const { return m_timeToReduceBodies; }
    Seconds timeToSolve() const { return m_timeToSolve; }

private:
    const Program& m_program;
    // By function: ValueFlowInstancedFunctions::findParametersThatAreKeys().
    Vector<uint32_t> m_positionOfKey;
    ValueFlowBodies m_bodies;
    ValueSetMaker m_maker;
    mutable ValueSetMakerPool m_makerPool;
    std::unique_ptr<ValueFlowInstancedFunctions> m_instancedFunctions;
    ValueFlowSolverInput m_input;
    std::unique_ptr<ValueFlowSolver> m_solver;
    bool m_hasResult { false };
    Seconds m_timeToReduceBodies;
    Seconds m_timeToSolve;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
