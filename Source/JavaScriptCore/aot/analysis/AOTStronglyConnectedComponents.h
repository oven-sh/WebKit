/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <algorithm>
#include <span>
#include <type_traits>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT { namespace Typed {

struct StronglyConnectedComponents {
    std::span<const uint32_t> membersOf(uint32_t component) const LIFETIME_BOUND { return members.subspan(firstMemberOf[component], firstMemberOf[component + 1] - firstMemberOf[component]); }

    Vector<uint32_t> componentOf;
    BitVector isInCycle;
    uint32_t numberOfComponents { 0 };
    // The nodes, component by component.
    Vector<uint32_t> members;
    Vector<uint32_t> firstMemberOf;
};

template<typename SuccessorsOf>
StronglyConnectedComponents computeStronglyConnectedComponents(unsigned numberOfNodes, NOESCAPE const SuccessorsOf& successorsOf)
{
    auto indexOf = [](const auto& element) -> unsigned {
        if constexpr (std::is_integral_v<std::remove_cvref_t<decltype(element)>>)
            return static_cast<unsigned>(element);
        else
            return element.index();
    };
    StronglyConnectedComponents result;
    result.componentOf.fill(0, numberOfNodes);
    result.isInCycle.ensureSize(numberOfNodes);
    result.members.reserveInitialCapacity(numberOfNodes);
    BitVector hasSelfEdge(numberOfNodes);
    BitVector isInComponentStack(numberOfNodes);
    BitVector isVisited(numberOfNodes);
    Vector<unsigned> order;
    order.fill(0, numberOfNodes);
    Vector<unsigned> lowest = order;
    Vector<unsigned> componentStack;
    Vector<std::pair<unsigned, unsigned>> walk;
    unsigned numberVisited = 0;
    for (unsigned root = 0; root < numberOfNodes; ++root) {
        if (isVisited.get(root))
            continue;
        walk.append({ root, 0 });
        while (!walk.isEmpty()) {
            auto [node, nextSuccessor] = walk.last();
            if (!nextSuccessor) {
                order[node] = lowest[node] = numberVisited++;
                isVisited.set(node);
                componentStack.append(node);
                isInComponentStack.set(node);
            }
            auto&& successors = successorsOf(node);
            if (nextSuccessor < successors.size()) {
                ++walk.last().second;
                unsigned successor = indexOf(successors[nextSuccessor]);
                if (successor == node)
                    hasSelfEdge.set(node);
                if (!isVisited.get(successor))
                    walk.append({ successor, 0 });
                else if (isInComponentStack.get(successor))
                    lowest[node] = std::min(lowest[node], order[successor]);
                continue;
            }
            walk.removeLast();
            if (!walk.isEmpty())
                lowest[walk.last().first] = std::min(lowest[walk.last().first], lowest[node]);
            if (lowest[node] != order[node])
                continue;
            bool isCycle = componentStack.last() != node || hasSelfEdge.get(node);
            result.firstMemberOf.append(result.members.size());
            for (;;) {
                unsigned member = componentStack.takeLast();
                isInComponentStack.clear(member);
                if (isCycle)
                    result.isInCycle.quickSet(member);
                result.componentOf[member] = result.numberOfComponents;
                result.members.append(member);
                if (member == node)
                    break;
            }
            ++result.numberOfComponents;
        }
    }
    result.firstMemberOf.append(result.members.size());
    return result;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
