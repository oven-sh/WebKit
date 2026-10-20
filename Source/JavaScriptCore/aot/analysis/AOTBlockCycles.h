/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTCFG.h"
#include <optional>
#include <wtf/CycleForest.h>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

class BlockCycles {
    WTF_MAKE_NONCOPYABLE(BlockCycles);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(BlockCycles);
public:
    using Forest = WTF::CycleForest<CFG>;

    explicit BlockCycles(CFG&);

    bool isInCycle(BlockID block) const
    {
        RELEASE_ASSERT(block.index() < m_outermost.size());
        return !!m_outermost[block.index()];
    }

    bool areInSameCycle(BlockID a, BlockID b) const { return isInCycle(a) && outermostCycleOf(a) == outermostCycleOf(b); }
    std::optional<unsigned> outermostCycleOf(BlockID block) const { return m_outermost[block.index()]; }
    std::optional<unsigned> innermostCycleOf(BlockID block) const
    {
        unsigned cycle = m_forest.innermost(block);
        if (cycle == Forest::none)
            return std::nullopt;
        return cycle;
    }

    // A definition that is not in every cycle of its use is used many times each time it runs, although the use is written once.
    bool isInEveryCycleOf(BlockID block, BlockID other) const
    {
        std::optional<unsigned> innermost = innermostCycleOf(other);
        return !innermost || m_forest.contains(*innermost, block);
    }

    unsigned depthOf(BlockID block) const { return m_forest.depth(m_forest.innermost(block)); }
    const Forest& forest() const LIFETIME_BOUND { return m_forest; }

private:
    Forest m_forest;
    Vector<std::optional<unsigned>> m_outermost;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
