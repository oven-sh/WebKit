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
#include <array>
#include <atomic>
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
    // A loop on many threads borrows a maker for each of its steps. What a thread gives back waits for that thread, where no other thread looks first.
    struct alignas(64) MakerOfThread {
        std::atomic<ValueSetMaker*> idleMaker { nullptr };
    };
    static constexpr unsigned numberOfThreadsToldApart = 64;
    MakerOfThread& makerOfCurrentThread();

    ValueSets& m_sets;
    std::array<MakerOfThread, numberOfThreadsToldApart> m_makersOfThreads;
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

class ValueFlowSolution {
    WTF_MAKE_NONCOPYABLE(ValueFlowSolution);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(ValueFlowSolution);
public:
    enum class Witnesses : uint8_t { None, ForEveryEdge };
    enum class WaysOfValues : uint8_t { AreAskedFor, AreNotAskedFor };
    enum class CoveredMembers : uint8_t { StayListed, AreLeftOut };
    ValueFlowSolution(Program&, BuildContext&, ValueFlowSolver::Rounds = ValueFlowSolver::Rounds::Due, Witnesses = Witnesses::None, uint32_t mostRounds = UINT32_MAX, EnumSet<ValueFlowUnknownSource> unknownSourcesLeftOut = { }, WaysOfValues = WaysOfValues::AreAskedFor, CoveredMembers = CoveredMembers::StayListed);
    ~ValueFlowSolution();

    const Program& program() const { return m_program; }
    const ValueSets& sets() const { return m_bodies.sets(); }
    const ValueFlowBodies& bodies() const { return m_bodies; }
    const ValueFlowSolver& solver() const { return *m_solver; }
    void letGoOfRecordsAskedFor() { m_solver->letGoOfRecordsAskedFor(); }
    // Where the build widens flow, solver() was given more to begin with, and this one solved the program as it is.
    const ValueFlowSolver* solverOfProgramAsItIs() const { return m_solverOfProgramAsItIs.get(); }
    ValueSetMakerPool& makerPool() const { return m_makerPool; }
    ValueSetID possibleValuesOf(const ValueFlowOperand&);

private:
    const Program& m_program;
    Vector<uint32_t> m_positionOfKey;
    ValueFlowBodies m_bodies;
    ValueSetMaker m_maker;
    mutable ValueSetMakerPool m_makerPool;
    std::unique_ptr<ValueFlowInstancedFunctions> m_instancedFunctions;
    ValueFlowSolverInput m_input;
    ValueFlowSolverInput m_inputAsItIs;
    std::unique_ptr<ValueFlowSolver> m_solverOfProgramAsItIs;
    std::unique_ptr<ValueFlowSolver> m_solver;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
