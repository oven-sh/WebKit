/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTDominators.h"
#include <optional>
#include <wtf/FastMalloc.h>
#include <wtf/NaturalLoops.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

using NaturalLoop = WTF::NaturalLoop<CFG>;

class NaturalLoops : public WTF::NaturalLoops<CFG> {
    WTF_MAKE_NONCOPYABLE(NaturalLoops);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(NaturalLoops);
    using Base = WTF::NaturalLoops<CFG>;
public:
    NaturalLoops(CFG& cfg, Dominators& dominators, bool selfCheck)
        : Base(cfg, dominators, selfCheck)
        , m_cfg(cfg)
    {
    }

    const NaturalLoop* headerOf(BlockID block) const
    {
        if (!m_cfg.isReachable(block))
            return nullptr;
        return Base::headerOf(block);
    }

    const NaturalLoop* innerMostLoopOf(BlockID block) const
    {
        if (!m_cfg.isReachable(block))
            return nullptr;
        return Base::innerMostLoopOf(block);
    }

    bool belongsTo(BlockID block, const NaturalLoop& candidateLoop) const
    {
        if (!m_cfg.isReachable(block))
            return false;
        return Base::belongsTo(block, candidateLoop);
    }

    unsigned loopDepth(BlockID block) const
    {
        if (!m_cfg.isReachable(block))
            return 0;
        return Base::loopDepth(block);
    }

    Vector<const NaturalLoop*> loopsOf(BlockID block) const
    {
        if (!m_cfg.isReachable(block))
            return { };
        return Base::loopsOf(block);
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
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
