/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTSort.h"
#include "AOTVector.h"
#include <algorithm>
#include <ranges>
#include <span>

namespace JSC { namespace AOT {

// For each node, in ascending order: the listed nodes from which the node is reached by going up.
// The list of a node is a stretch of the list of the node above it wherever no other member of that list lies between its own: down a long row of nodes all the lists are one.
template<typename ID>
class ListsBelow {
public:
    ListsBelow() = default;

    ListsBelow(uint32_t numberOfNodes, NOESCAPE const Invocable<bool(ID)> auto& isListed, NOESCAPE const Invocable<ID(ID)> auto& nodeAbove)
        : m_stretches(numberOfNodes)
    {
        Vector<uint32_t> above(numberOfNodes, [&](size_t index) {
            ID node = nodeAbove(ID(static_cast<uint32_t>(index)));
            return node && node.index() != index ? static_cast<uint32_t>(node.index()) : none;
        });
        Vector<uint32_t> firstRightBelow(FillWith { }, numberOfNodes + 1, 0u);
        for (uint32_t node : above) {
            if (node != none)
                ++firstRightBelow[node + 1];
        }
        for (uint32_t index = 0; index < numberOfNodes; ++index)
            firstRightBelow[index + 1] += firstRightBelow[index];
        if (!firstRightBelow[numberOfNodes])
            return;

        Vector<uint32_t> fromLowest;
        fromLowest.reserveInitialCapacity(numberOfNodes);
        Vector<uint32_t> numberLeftRightBelow(numberOfNodes, [&](size_t index) {
            return firstRightBelow[index + 1] - firstRightBelow[index];
        });
        for (uint32_t index = 0; index < numberOfNodes; ++index) {
            if (!numberLeftRightBelow[index])
                fromLowest.append(index);
        }
        for (size_t next = 0; next < fromLowest.size(); ++next) {
            uint32_t node = above[fromLowest[next]];
            if (node != none && !--numberLeftRightBelow[node])
                fromLowest.append(node);
        }
        if (fromLowest.size() < numberOfNodes) {
            listWhereNodesStandInRings(isListed, nodeAbove);
            return;
        }

        Vector<uint32_t> rightBelow(firstRightBelow[numberOfNodes]);
        Vector<uint32_t> nextRightBelow(firstRightBelow.span().first(numberOfNodes));
        for (uint32_t index = 0; index < numberOfNodes; ++index) {
            if (above[index] != none)
                rightBelow[nextRightBelow[above[index]]++] = index;
        }

        struct Listed {
            uint32_t count { 0 };
            uint32_t lowest { none };
            uint32_t highest { 0 };
        };
        Vector<Listed> listedBelow(numberOfNodes);
        for (uint32_t index : fromLowest) {
            if (above[index] == none)
                continue;
            Listed listed = listedBelow[index];
            if (isListed(ID(index))) {
                ++listed.count;
                listed.lowest = std::min(listed.lowest, index);
                listed.highest = std::max(listed.highest, index);
            }
            Listed& sum = listedBelow[above[index]];
            sum.count += listed.count;
            sum.lowest = std::min(sum.lowest, listed.lowest);
            sum.highest = std::max(sum.highest, listed.highest);
        }

        Vector<uint32_t> worklist;
        for (uint32_t index : fromLowest | std::views::reverse) {
            const Listed& listed = listedBelow[index];
            if (!listed.count)
                continue;
            if (above[index] != none) {
                std::span<const ID> listAbove = (*this)[above[index]];
                size_t position = std::ranges::lower_bound(listAbove, ID(listed.lowest)) - listAbove.begin();
                if (position + listed.count <= listAbove.size() && listAbove[position + listed.count - 1] == ID(listed.highest)) {
                    m_stretches[index] = { m_stretches[above[index]].start + static_cast<uint32_t>(position), listed.count };
                    continue;
                }
            }
            uint32_t start = static_cast<uint32_t>(m_nodes.size());
            worklist.append(index);
            while (!worklist.isEmpty()) {
                uint32_t node = worklist.takeLast();
                for (uint32_t below : rightBelow.subspan(firstRightBelow[node], firstRightBelow[node + 1] - firstRightBelow[node])) {
                    if (isListed(ID(below)))
                        m_nodes.append(ID(below));
                    worklist.append(below);
                }
            }
            AOT::sort(m_nodes.mutableSpan().subspan(start));
            RELEASE_ASSERT(m_nodes.size() - start == listed.count);
            m_stretches[index] = { start, listed.count };
        }
    }

    std::span<const ID> operator[](size_t index) const LIFETIME_BOUND
    {
        if (index >= m_stretches.size())
            return { };
        return m_nodes.subspan(m_stretches[index].start, m_stretches[index].size);
    }

private:
    static constexpr uint32_t none = UINT32_MAX;

    struct Stretch {
        uint32_t start { 0 };
        uint32_t size { 0 };
    };

    void listWhereNodesStandInRings(NOESCAPE const Invocable<bool(ID)> auto& isListed, NOESCAPE const Invocable<ID(ID)> auto& nodeAbove)
    {
        Vector<Vector<ID>> lists(m_stretches.size());
        for (uint32_t index = 0; index < m_stretches.size(); ++index) {
            ID node(index);
            if (!isListed(node))
                continue;
            for (ID current = nodeAbove(node); current && current != node; current = nodeAbove(current)) {
                Vector<ID>& below = lists[current.index()];
                bool isOnRing = !below.isEmpty() && below.last() == node;
                if (isOnRing)
                    break;
                below.append(node);
            }
        }
        for (uint32_t index = 0; index < m_stretches.size(); ++index) {
            m_stretches[index] = { static_cast<uint32_t>(m_nodes.size()), static_cast<uint32_t>(lists[index].size()) };
            m_nodes.appendVector(lists[index]);
        }
    }

    Vector<ID> m_nodes;
    Vector<Stretch> m_stretches;
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
