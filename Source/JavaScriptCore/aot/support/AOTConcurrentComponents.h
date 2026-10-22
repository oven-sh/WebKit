/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <algorithm>
#include <atomic>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

class ConcurrentComponents {
public:
    ConcurrentComponents() = default;

    explicit ConcurrentComponents(uint32_t numberOfMembers)
        : m_parents(numberOfMembers)
    {
        for (uint32_t member = 0; member < numberOfMembers; ++member)
            m_parents[member].store(member, std::memory_order_relaxed);
    }

    uint32_t parentOf(uint32_t member) const { return m_parents[member].load(std::memory_order_relaxed); }

    uint32_t rootAbove(uint32_t member) const
    {
        while (true) {
            uint32_t parent = parentOf(member);
            if (parent == member)
                return member;
            member = parent;
        }
    }

    void shortenPath(uint32_t member, uint32_t root)
    {
        while (member > root)
            member = m_parents[member].exchange(root, std::memory_order_relaxed);
    }

    uint32_t componentOf(uint32_t member)
    {
        uint32_t root = rootAbove(member);
        shortenPath(member, root);
        return root;
    }

    void join(uint32_t member, uint32_t otherMember)
    {
        while (true) {
            uint32_t root = componentOf(member);
            uint32_t otherRoot = componentOf(otherMember);
            if (root == otherRoot)
                return;
            uint32_t higher = std::max(root, otherRoot);
            uint32_t expected = higher;
            if (m_parents[higher].compare_exchange_strong(expected, std::min(root, otherRoot), std::memory_order_relaxed))
                return;
        }
    }

private:
    Vector<std::atomic<uint32_t>> m_parents;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
