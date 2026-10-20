/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

struct DataflowStep {
    BlockID block;
    uint32_t index { 0 };
    ValueID id;
    const Value& value;
    const EffectsOnEdges& effects;
};

struct DataflowTerminatorStep {
    BlockID block;
    const Terminator& terminator;
    const EffectsOnEdges& effects;
};

template<typename Problem>
concept HasThrowTransfer = requires(Problem& problem, typename Problem::State& state, const DataflowStep& step) {
    problem.executeThrow(state, step);
};

template<typename Problem>
class ForwardDataflowSolver {
    WTF_MAKE_NONCOPYABLE(ForwardDataflowSolver);
public:
    using State = typename Problem::State;
    using Heads = Vector<std::optional<State>>;

    ForwardDataflowSolver(FunctionAnalyses& analyses, Problem& problem)
        : m_analyses(analyses)
        , m_function(analyses.function())
        , m_cfg(analyses.cfg())
        , m_problem(problem)
        , m_heads(m_function.blocks.size())
        , m_isPending(m_cfg.reversePostOrder().size())
    {
    }

    Heads run()
    {
        BlockID root = m_cfg.root();
        m_heads[root.index()] = m_problem.stateAtEntry();
        m_isPending.quickSet(m_cfg.reversePostOrderIndex(root));
        auto order = m_cfg.reversePostOrder();
        for (bool shouldRepeat = true; shouldRepeat;) {
            shouldRepeat = false;
            for (uint32_t blockOrder = 0; blockOrder < order.size(); ++blockOrder) {
                if (m_isPending.quickClear(blockOrder))
                    shouldRepeat |= visit(order[blockOrder], blockOrder);
            }
        }
        return WTF::move(m_heads);
    }

private:
    struct HandlerEntry {
        State state;
        bool includesCurrentState { true };
    };

    void meetCurrentState(HandlerEntry& entry, const State& state)
    {
        if (entry.includesCurrentState)
            return;
        m_problem.merge(entry.state, state);
        entry.includesCurrentState = true;
    }

    void executeCoveredValue(State& state, HandlerEntry& entry, const DataflowStep& step)
    {
        bool mayThrow = step.effects.mayThrow();
        if constexpr (HasThrowTransfer<Problem>) {
            if (mayThrow) {
                State onThrow = state;
                m_problem.executeThrow(onThrow, step);
                m_problem.merge(entry.state, onThrow);
            }
            m_problem.execute(state, step);
            entry.includesCurrentState = false;
        } else {
            if (mayThrow)
                meetCurrentState(entry, state);
            m_problem.execute(state, step);
            entry.includesCurrentState = false;
            if (mayThrow)
                meetCurrentState(entry, state);
        }
    }

    void executeCoveredTerminator(State& state, HandlerEntry& entry, const DataflowTerminatorStep& step)
    {
        bool mayThrow = step.effects.mayThrow();
        if (mayThrow)
            meetCurrentState(entry, state);
        m_problem.executeTerminator(state, step);
        entry.includesCurrentState = false;
        if (mayThrow)
            meetCurrentState(entry, state);
    }

    bool propagate(Edge edge, State&& state, uint32_t fromOrder)
    {
        m_problem.enterEdge(state, edge);
        BlockID target = targetOf(m_function, edge).block;
        std::optional<State>& head = m_heads[target.index()];
        if (head) {
            if (!m_problem.merge(*head, state))
                return false;
        } else
            head = WTF::move(state);
        uint32_t targetOrder = m_cfg.reversePostOrderIndex(target);
        m_isPending.quickSet(targetOrder);
        return targetOrder <= fromOrder;
    }

    bool visit(BlockID id, uint32_t blockOrder)
    {
        const Block& block = m_function.at(id);
        State state = *m_heads[id.index()];
        std::optional<HandlerEntry> handlerEntry;
        if (block.handler)
            handlerEntry = HandlerEntry { state };

        for (uint32_t index = 0; index < block.values.size(); ++index) {
            ValueID value = block.values[index];
            DataflowStep step { id, index, value, m_function.at(value), m_analyses.effectsOf(value) };
            if (handlerEntry)
                executeCoveredValue(state, *handlerEntry, step);
            else
                m_problem.execute(state, step);
        }

        DataflowTerminatorStep terminatorStep { id, block.terminator, m_analyses.effectsOfTerminator(id) };
        if (handlerEntry)
            executeCoveredTerminator(state, *handlerEntry, terminatorStep);
        else
            m_problem.executeTerminator(state, terminatorStep);

        bool shouldRepeat = false;
        uint32_t numberOfTargets = block.terminator.targets.size();
        for (uint32_t targetIndex = 0; targetIndex < numberOfTargets; ++targetIndex) {
            bool isLastTarget = targetIndex + 1 == numberOfTargets;
            shouldRepeat |= propagate(Edge { id, targetIndex }, isLastTarget ? WTF::move(state) : State(state), blockOrder);
        }
        if (handlerEntry)
            shouldRepeat |= propagate(Edge::toHandlerOf(id), WTF::move(handlerEntry->state), blockOrder);
        return shouldRepeat;
    }

    FunctionAnalyses& m_analyses;
    const Function& m_function;
    const CFG& m_cfg;
    Problem& m_problem;
    Heads m_heads;
    BitVector m_isPending;
};

template<typename Problem>
Vector<std::optional<typename Problem::State>> FunctionAnalyses::solveForward(Problem& problem)
{
    return ForwardDataflowSolver<Problem>(*this, problem).run();
}

template<typename Problem>
void replayBlock(FunctionAnalyses& analyses, Problem& problem, typename Problem::State& state, BlockID id, uint32_t endIndex)
{
    const Function& function = analyses.function();
    const Block& block = function.at(id);
    RELEASE_ASSERT(endIndex <= block.values.size());
    for (uint32_t index = 0; index < endIndex; ++index) {
        ValueID value = block.values[index];
        problem.execute(state, DataflowStep { id, index, value, function.at(value), analyses.effectsOf(value) });
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
