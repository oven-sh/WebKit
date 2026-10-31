/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"
#include "AOTVector.h"
#include <concepts>
#include <optional>
#include "AOTBitVector.h"

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
concept HasStatesThatIgnoreEffects = requires(const Problem& problem, const typename Problem::State& state) {
    { problem.ignoresEffects(state) } -> std::same_as<bool>;
};

template<typename Problem>
concept ReadsNoEffects = requires {
    requires !Problem::readsEffects();
};

template<typename Problem>
concept ExecutesValuesOfBlock = requires(Problem& problem, typename Problem::State& state, BlockID block) {
    problem.executeValuesOf(state, block);
};

inline constexpr EffectsOnEdges noEffectsOnEdges = EffectsOnEdges::none();

template<typename Problem>
Vector<std::optional<typename Problem::State>> FunctionAnalyses::solveForward(Problem& problem)
{
    using State = typename Problem::State;
    struct HandlerEntry {
        State state;
        bool includesCurrentState { true };
    };

    const Function& function = this->function();
    const CFG& cfg = this->cfg();
    Vector<std::optional<State>> heads(function.blocks.size());
    BitVector isPending(cfg.reversePostOrder().size());

    auto executeCovered = [&](State& state, HandlerEntry& entry, bool mayThrow, NOESCAPE const Invocable<void()> auto& execute) {
        auto meetCurrentState = [&] {
            if (!mayThrow || entry.includesCurrentState)
                return;
            problem.merge(entry.state, state);
            entry.includesCurrentState = true;
        };
        meetCurrentState();
        execute();
        entry.includesCurrentState = false;
        meetCurrentState();
    };
    auto propagate = [&](Edge edge, State&& state, uint32_t fromOrder) {
        problem.enterEdge(state, edge);
        BlockID target = targetOf(function, edge).block;
        std::optional<State>& head = heads[target.index()];
        if (head) {
            if (!problem.merge(*head, state))
                return false;
        } else
            head = WTF::move(state);
        uint32_t targetOrder = cfg.reversePostOrderIndex(target);
        isPending.quickSet(targetOrder);
        return targetOrder <= fromOrder;
    };
    auto visit = [&](BlockID id, uint32_t blockOrder) {
        const Block& block = function.at(id);
        State state = *heads[id.index()];
        std::optional<HandlerEntry> handlerEntry;
        if (block.handler)
            handlerEntry = HandlerEntry { state };

        auto readsNoEffects = [&] {
            if (ReadsNoEffects<Problem> && !handlerEntry)
                return true;
            if constexpr (HasStatesThatIgnoreEffects<Problem>)
                return problem.ignoresEffects(state);
            else
                return false;
        };
        uint32_t firstIndex = 0;
        if constexpr (ExecutesValuesOfBlock<Problem>) {
            if (!handlerEntry) {
                problem.executeValuesOf(state, id);
                firstIndex = block.values.size();
            }
        }
        for (uint32_t index = firstIndex; index < block.values.size(); ++index) {
            ValueID value = block.values[index];
            DataflowStep step { id, index, value, function.at(value), readsNoEffects() ? noEffectsOnEdges : effectsOf(value) };
            if (!handlerEntry) {
                problem.execute(state, step);
                continue;
            }
            if constexpr (HasThrowTransfer<Problem>) {
                if (step.effects.mayThrow()) {
                    State onThrow = state;
                    problem.executeThrow(onThrow, step);
                    problem.merge(handlerEntry->state, onThrow);
                }
                problem.execute(state, step);
                handlerEntry->includesCurrentState = false;
            } else {
                executeCovered(state, *handlerEntry, step.effects.mayThrow(), [&] {
                    problem.execute(state, step);
                });
            }
        }

        DataflowTerminatorStep terminatorStep { id, block.terminator, readsNoEffects() ? noEffectsOnEdges : effectsOfTerminator(id) };
        if (handlerEntry) {
            executeCovered(state, *handlerEntry, terminatorStep.effects.mayThrow(), [&] {
                problem.executeTerminator(state, terminatorStep);
            });
        } else
            problem.executeTerminator(state, terminatorStep);

        bool shouldRepeat = false;
        uint32_t numberOfTargets = block.terminator.targets.size();
        for (uint32_t targetIndex = 0; targetIndex < numberOfTargets; ++targetIndex) {
            bool isLastTarget = targetIndex + 1 == numberOfTargets;
            shouldRepeat |= propagate(Edge { id, targetIndex }, isLastTarget ? WTF::move(state) : State(state), blockOrder);
        }
        if (handlerEntry)
            shouldRepeat |= propagate(Edge::toHandlerOf(id), WTF::move(handlerEntry->state), blockOrder);
        return shouldRepeat;
    };

    heads[cfg.root().index()] = problem.stateAtEntry();
    isPending.quickSet(cfg.reversePostOrderIndex(cfg.root()));
    auto order = cfg.reversePostOrder();
    for (bool shouldRepeat = true; shouldRepeat;) {
        shouldRepeat = false;
        for (uint32_t blockOrder = 0; blockOrder < order.size(); ++blockOrder) {
            if (isPending.quickClear(blockOrder))
                shouldRepeat |= visit(order[blockOrder], blockOrder);
        }
    }
    return heads;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
