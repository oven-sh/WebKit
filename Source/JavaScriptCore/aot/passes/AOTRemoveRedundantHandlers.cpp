/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include <algorithm>
#include <optional>
#include <utility>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

class RedundantHandlerRemover {
public:
    explicit RedundantHandlerRemover(FunctionPassContext& context)
        : m_function(context.function)
        , m_analyses(context.analyses)
        , m_builder(context.analyses.context(), context.function, context.types, &context.analyses)
    {
    }

    PassOutcome run()
    {
        size_t numberOfChanges = 0;
        for (;;) {
            size_t changesInRound = removeUnusedHandlers();
            changesInRound += skipRethrowingHandlers();
            if (!changesInRound)
                break;
            numberOfChanges += changesInRound;
        }
        if (!numberOfChanges)
            return PassOutcome::Unchanged;

        m_builder.emptyUnreachableBlocks();
        return PassOutcome::Changed;
    }

private:
    bool mayReachHandler(BlockID block)
    {
        bool hasThrowingValue = std::ranges::any_of(m_function.at(block).values, [&](ValueID value) {
            return m_analyses.mayThrow(value);
        });
        return hasThrowingValue || m_analyses.effectsOfTerminator(block).mayThrow();
    }

    size_t removeUnusedHandlers()
    {
        Vector<BlockID, 8> blocks;
        for (BlockID block : m_analyses.cfg().reversePostOrder()) {
            if (m_function.at(block).handler && !mayReachHandler(block))
                blocks.append(block);
        }
        for (BlockID block : blocks)
            m_builder.removeHandler(block);
        return blocks.size();
    }

    size_t skipRethrowingHandlers()
    {
        collectHandlers();

        size_t numberOfSkippedHandlers = 0;
        bool changedInWalk;
        do {
            changedInWalk = false;
            for (BlockID handler : m_handlers) {
                if (rewriteSources(handler)) {
                    ++numberOfSkippedHandlers;
                    changedInWalk = true;
                }
            }
            if (changedInWalk)
                m_analyses.didChangeControlFlow();
        } while (changedInWalk);
        return numberOfSkippedHandlers;
    }

    bool doesNothing(ValueID id)
    {
        return m_analyses.effectsOf(id).onAnyEdge().isRemovableWhenUnused();
    }

    bool rethrowsCaughtValue(const Block& block) const
    {
        if (!block.isHandlerBlock() || !block.parameters.isEmpty() || block.values.isEmpty())
            return false;

        ValueID caught = block.values[0];
        Opcode opcode = m_function.at(caught).opcode;
        const Terminator& terminator = block.terminator;
        if (opcode == Opcode::CatchValue)
            return terminator.kind == TerminatorKind::Throw && terminator.valueOperand() == caught;
        if (opcode == Opcode::CatchException)
            return terminator.kind == TerminatorKind::Rethrow && terminator.exceptionOperand() == caught;
        return false;
    }

    bool isUsedOutside(BlockID id, const Block& block, const UseLists& useLists) const
    {
        const std::optional<Target>& outer = block.handler;
        return std::ranges::any_of(block.values, [&](ValueID value) {
            if (outer && outer->arguments.contains(value))
                return true;
            return std::ranges::any_of(useLists.usesOf(value), [&](const Use& use) {
                return use.block != id;
            });
        });
    }

    bool onlyRethrows(BlockID id, const UseLists& useLists)
    {
        const Block& block = m_function.at(id);
        if (!rethrowsCaughtValue(block))
            return false;
        for (size_t index = 1; index < block.values.size(); ++index) {
            if (!doesNothing(block.values[index]))
                return false;
        }
        return !isUsedOutside(id, block, useLists);
    }

    void collectHandlers()
    {
        m_handlers.shrink(0);
        m_incomingEdges.shrink(0);

        const CFG& cfg = m_analyses.cfg();
        if (!cfg.hasHandlers())
            return;

        const UseLists& useLists = m_analyses.useLists();
        for (uint32_t index = 0; index < m_function.blocks.size(); ++index) {
            BlockID block(index);
            if (cfg.isReachable(block) && onlyRethrows(block, useLists))
                m_handlers.append(block);
        }
        if (m_handlers.isEmpty())
            return;
        m_incomingEdges.grow(m_function.blocks.size());
        for (BlockID handler : m_handlers)
            m_incomingEdges[handler.index()].append(cfg.incomingEdges(handler));
    }

    bool hasOnlyHandlerEdges(BlockID handler) const
    {
        return std::ranges::all_of(m_incomingEdges[handler.index()], [](const Edge& edge) {
            return edge.isHandlerEdge();
        });
    }

    void rewriteSource(Edge edge, const std::optional<Target>& outer)
    {
        if (!outer) {
            m_builder.removeHandler(edge.from);
            return;
        }
        m_function.at(edge.from).handler = *outer;
        m_incomingEdges[outer->block.index()].append(edge);
    }

    bool rewriteSources(BlockID handler)
    {
        std::optional<Target> outer = m_function.at(handler).handler;
        if (outer && outer->block == handler)
            return false;
        RELEASE_ASSERT(hasOnlyHandlerEdges(handler));

        bool lostEdge = false;
        for (const Edge& edge : std::exchange(m_incomingEdges[handler.index()], { })) {
            if (edge.from == handler) {
                m_incomingEdges[handler.index()].append(edge);
                continue;
            }
            rewriteSource(edge, outer);
            lostEdge = true;
        }
        return lostEdge;
    }

    Function& m_function;
    FunctionAnalyses& m_analyses;
    IRBuilder m_builder;
    Vector<BlockID, 4> m_handlers;
    Vector<Vector<Edge, 2>> m_incomingEdges;
};

} // anonymous namespace

PassOutcome removeRedundantHandlers(FunctionPassContext& context)
{
    if (!context.analyses.cfg().hasHandlers())
        return PassOutcome::Unchanged;
    return RedundantHandlerRemover(context).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
