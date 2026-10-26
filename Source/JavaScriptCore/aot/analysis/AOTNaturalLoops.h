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
public:
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
    const NaturalLoop* headerOf(BlockID block) const { return mayBeInLoop(block) ? m_loops->headerOf(block) : nullptr; }
    const NaturalLoop* innerMostLoopOf(BlockID block) const { return mayBeInLoop(block) ? m_loops->innerMostLoopOf(block) : nullptr; }
    const NaturalLoop* innerMostOuterLoop(const NaturalLoop& loop) const { return m_loops->innerMostOuterLoop(loop); }
    bool belongsTo(BlockID block, const NaturalLoop& candidateLoop) const { return m_cfg.isReachable(block) && m_loops->belongsTo(block, candidateLoop); }
    unsigned loopDepth(BlockID block) const { return mayBeInLoop(block) ? m_loops->loopDepth(block) : 0; }

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
    bool mayBeInLoop(BlockID block) const { return m_loops && m_cfg.isReachable(block); }

    const CFG& m_cfg;
    std::optional<WTF::NaturalLoops<CFG>> m_loops;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
