/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBlockCycles.h"

#if ENABLE(AOT)

#include <algorithm>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

BlockCycles::BlockCycles(CFG& cfg)
    : m_forest(cfg, [&cfg](std::span<const BlockID> members, const Vector<BlockID, 2>& entries, const auto&) -> BlockID {
        std::span<const BlockID> candidates = entries.isEmpty() ? members : entries.span();
        return *std::ranges::min_element(candidates, { }, [&cfg](BlockID block) {
            return cfg.reversePostOrderIndex(block);
        });
    })
    , m_outermost(cfg.numNodes())
{
    for (BlockID block : cfg.reversePostOrder()) {
        unsigned cycle = m_forest.innermost(block);
        if (cycle == Forest::none)
            continue;
        while (m_forest[cycle].parent != Forest::none)
            cycle = m_forest[cycle].parent;
        m_outermost[block.index()] = cycle;
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
