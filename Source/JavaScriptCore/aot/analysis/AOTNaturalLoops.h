/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTDominators.h"
#include "AOTVector.h"
#include <optional>
#include <wtf/FastMalloc.h>
#include <wtf/NaturalLoops.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

using NaturalLoop = WTF::NaturalLoop<CFG>;

class NaturalLoops {
    WTF_MAKE_NONCOPYABLE(NaturalLoops);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(NaturalLoops);
    using Loops = WTF::NaturalLoops<CFG>;
public:
    // The edge that closes a loop closes a cycle. In a graph without a cycle nobody needs the dominators to know that there is no loop.
    NaturalLoops(CFG& cfg, Dominators& dominators, bool selfCheck)
        : m_cfg(cfg)
    {
        if (!cfg.hasCycle() && !selfCheck)
            return;
        m_loops.emplace(cfg, dominators, selfCheck);
        RELEASE_ASSERT(cfg.hasCycle() || !m_loops->numLoops());
    }

    unsigned numLoops() const { return m_loops ? m_loops->numLoops() : 0; }
    const NaturalLoop& loop(unsigned index) const LIFETIME_BOUND { return m_loops->loop(index); }

    const NaturalLoop* headerOf(BlockID block) const
    {
        if (!m_loops || !m_cfg.isReachable(block))
            return nullptr;
        return m_loops->headerOf(block);
    }

    const NaturalLoop* innerMostLoopOf(BlockID block) const
    {
        if (!m_loops || !m_cfg.isReachable(block))
            return nullptr;
        return m_loops->innerMostLoopOf(block);
    }

    const NaturalLoop* innerMostOuterLoop(const NaturalLoop& loop) const { return m_loops->innerMostOuterLoop(loop); }

    bool belongsTo(BlockID block, const NaturalLoop& candidateLoop) const
    {
        if (!m_cfg.isReachable(block))
            return false;
        return m_loops->belongsTo(block, candidateLoop);
    }

    unsigned loopDepth(BlockID block) const
    {
        if (!m_loops || !m_cfg.isReachable(block))
            return 0;
        return m_loops->loopDepth(block);
    }

    Vector<const NaturalLoop*> loopsOf(BlockID block) const
    {
        if (!m_loops || !m_cfg.isReachable(block))
            return { };
        return m_loops->loopsOf(block);
    }

    std::optional<Edge> findSingleEntry(const NaturalLoop& loop) const
    {
        std::optional<Edge> entry;
        for (Edge edge : m_cfg.incomingEdges(loop.header())) {
            if (belongsTo(edge.from, loop))
                continue;
            if (entry)
                return std::nullopt;
            entry = edge;
        }
        if (entry && entry->isHandlerEdge())
            return std::nullopt;
        return entry;
    }

private:
    const CFG& m_cfg;
    std::optional<Loops> m_loops;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
