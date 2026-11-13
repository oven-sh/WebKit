/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class DominatorJumps {
public:
    DominatorJumps(uint32_t numberOfNodes, uint32_t root = 0)
        : m_root(root)
        , m_nodes(FillWith { }, numberOfNodes, Node { 0, root })
    {
    }

    uint32_t from(uint32_t node) const { return m_isStopped ? m_root : m_nodes[node].jump; }

    void set(uint32_t node, uint32_t dominator)
    {
        const Node& above = m_nodes[dominator];
        const Node& farther = m_nodes[above.jump];
        bool mayJumpTwice = above.depth - farther.depth == farther.depth - m_nodes[farther.jump].depth;
        m_nodes[node] = { above.depth + 1, mayJumpTwice ? farther.jump : dominator };
    }

    void stop() { m_isStopped = true; }

private:
    struct Node {
        uint32_t depth;
        uint32_t jump;
    };

    uint32_t m_root;
    bool m_isStopped { false };
    Vector<Node> m_nodes;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
