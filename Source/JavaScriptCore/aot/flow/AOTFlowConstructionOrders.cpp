/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowConstruction.h"

#if ENABLE(AOT)

#include "AOTFlowKeys.h"
#include <algorithm>
#include <optional>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct InitialOrder {
    Vector<FlowConstructionName> names;
    FlowConstructionStores stores { FlowConstructionStores::Unknown };
};

class InitialOrderBuilder {
public:
    InitialOrderBuilder(const FlowConstructionGraph& graph, const FlowConstructionNode& node)
        : m_graph(graph)
        , m_node(node)
        , m_summary(*node.summary)
        , m_analyses(node.function->analyses)
        , m_program(m_analyses.program())
        , m_cfg(m_analyses.cfg())
    {
    }

    InitialOrder build(ThisLeak leak)
    {
        m_reachableAfter.fill(std::nullopt, m_analyses.function().blocks.size());
        collectOrderPoints();

        InitialOrder order;
        bool everyPointAccepted = true;
        for (uint32_t index = 0; index < m_creatingPoints.size(); ++index) {
            uint32_t position = m_creatingPoints[index];
            if (!isOrderCandidate(position)) {
                everyPointAccepted = false;
                m_passedOverPoints.append(position);
                continue;
            }
            if (!isOrderPointAccepted(index) || appendCreatedNames(position, order.names) == FlowConstructionStores::Unknown) {
                everyPointAccepted = false;
                break;
            }
        }
        if (leak == ThisLeak::DoesNotLeak && everyPointAccepted)
            order.stores = FlowConstructionStores::OnlyInitialOrder;
        return order;
    }

private:
    const FlowConstructionEvent& eventAt(uint32_t position) const { return m_summary.events[position]; }
    uint32_t positionOf(const FlowConstructionEvent& event) const { return static_cast<uint32_t>(&event - m_summary.events.span().data()); }
    const FlowResolvedConstructionEvent& resolvedAt(uint32_t position) const { return m_node.resolvedEvents[position]; }
    const FlowConstructionName& nameAt(uint32_t position) const { return m_summary.names[*eventAt(position).name]; }
    const FlowSolvedConstruction& solvedOf(uint32_t callee) const { return m_graph.nodes[callee].solved; }

    bool createsOwnKey(uint32_t position) const
    {
        if (eventAt(position).kind != FlowConstructionEventKind::Store || !resolvedAt(position).doesNotLeak)
            return false;
        PropertyKeyID key = nameAt(position).key;
        return key && classOf(m_program, key) != FlowKeyClass::PrivateName;
    }

    bool mayCreateKeys(uint32_t position) const
    {
        return createsOwnKey(position) || std::ranges::any_of(resolvedAt(position).callees, [&](uint32_t callee) {
            const FlowSolvedConstruction& solved = solvedOf(callee);
            return solved.stores != FlowConstructionStores::OnlyInitialOrder || !solved.initialOrder.isEmpty();
        });
    }

    bool mayLeak(uint32_t position) const
    {
        const FlowResolvedConstructionEvent& resolved = resolvedAt(position);
        return !resolved.doesNotLeak || std::ranges::any_of(resolved.callees, [&](uint32_t callee) {
            return solvedOf(callee).leak == ThisLeak::Unknown;
        });
    }

    bool isSoleCreatingPoint(uint32_t position) const
    {
        return std::ranges::all_of(m_summary.eventsAt(eventAt(position).point), [&](const FlowConstructionEvent& other) {
            return positionOf(other) == position || !mayCreateKeys(positionOf(other));
        });
    }

    bool mustCreateKeys(uint32_t position) const
    {
        const FlowConstructionEvent& event = eventAt(position);
        if (!event.isExact || !resolvedAt(position).runsOneCallee || !isSoleCreatingPoint(position))
            return false;
        return resolvedAt(position).callees.isEmpty() || event.kind == FlowConstructionEventKind::Call;
    }

    void collectOrderPoints()
    {
        std::span<const FlowConstructionEvent> events = m_summary.events.span();
        for (BlockID block : m_cfg.reversePostOrder()) {
            auto eventsOfBlock = std::ranges::equal_range(events, block, { }, [](const FlowConstructionEvent& event) {
                return event.point.block;
            });
            uint32_t firstCreatingPoint = m_creatingPoints.size();
            for (const FlowConstructionEvent& event : eventsOfBlock) {
                uint32_t position = positionOf(event);
                if (mayCreateKeys(position))
                    m_creatingPoints.append(position);
                if (mayLeak(position))
                    m_leakPoints.append(position);
                if (event.kind == FlowConstructionEventKind::Exit)
                    m_exitPoints.append(position);
            }
            if (firstCreatingPoint < m_creatingPoints.size())
                m_creatingBlocks.append({ block, firstCreatingPoint, static_cast<uint32_t>(m_creatingPoints.size()) });
        }
    }

    const BitVector& blocksReachableAfter(BlockID block)
    {
        std::optional<BitVector>& reachable = m_reachableAfter[block.index()];
        if (!reachable) {
            reachable = BitVector(m_reachableAfter.size());
            for (BlockID successor : m_cfg.successors(block))
                reachable->merge(m_cfg.blocksReachableFrom(successor, { }));
        }
        return *reachable;
    }

    bool reaches(uint32_t from, uint32_t to)
    {
        ProgramPoint fromPoint = eventAt(from).point;
        ProgramPoint toPoint = eventAt(to).point;
        if (fromPoint.block == toPoint.block && fromPoint.index < toPoint.index)
            return true;
        return blocksReachableAfter(fromPoint.block).get(toPoint.block.index());
    }

    bool precedesOnEveryPath(uint32_t position, uint32_t later)
    {
        ProgramPoint point = eventAt(position).point;
        ProgramPoint laterPoint = eventAt(later).point;
        if (point.block == laterPoint.block)
            return point.index < laterPoint.index;
        return m_analyses.dominators().terminatorDominates(point.block, laterPoint.block);
    }

    bool isOrderCandidate(uint32_t position)
    {
        auto comesLater = [&](uint32_t point) { return point == position || precedesOnEveryPath(position, point); };
        return mustCreateKeys(position) && !m_exitPoints.isEmpty() && std::ranges::all_of(m_exitPoints, comesLater) && std::ranges::all_of(m_leakPoints, comesLater);
    }

    bool storesOnlyAccepted(uint32_t point, uint32_t candidate) const
    {
        auto isAccepted = [&](const FlowConstructionName& name) {
            return m_acceptedNames.contains(name) || (resolvedAt(candidate).callees.isEmpty() && nameAt(candidate) == name);
        };
        if (createsOwnKey(point) && !isAccepted(nameAt(point)))
            return false;
        return std::ranges::all_of(resolvedAt(point).callees, [&](uint32_t callee) {
            const FlowSolvedConstruction& solved = solvedOf(callee);
            return solved.stores == FlowConstructionStores::OnlyInitialOrder && std::ranges::all_of(solved.initialOrder, isAccepted);
        });
    }

    // The points before this one are accepted or passed over. A point that has not had its turn comes later in its block or lies in a later block,
    // so it reaches this one only round a cycle, which is a question about its block: a constructor with thousands of stores in a row asks it once for each of them.
    bool isOrderPointAccepted(uint32_t index)
    {
        uint32_t position = m_creatingPoints[index];
        for (uint32_t leak : m_leakPoints) {
            if (reaches(leak, position))
                return false;
        }
        for (uint32_t point : m_passedOverPoints) {
            if (reaches(point, position) && !storesOnlyAccepted(point, position))
                return false;
        }

        BlockID block = eventAt(position).point.block;
        for (const CreatingBlock& creatingBlock : m_creatingBlocks) {
            if (creatingBlock.end <= index + 1 || !blocksReachableAfter(creatingBlock.block).get(block.index()))
                continue;
            for (uint32_t later = std::max(creatingBlock.begin, index + 1); later < creatingBlock.end; ++later) {
                if (!storesOnlyAccepted(m_creatingPoints[later], position))
                    return false;
            }
        }
        return true;
    }

    const FlowSolvedConstruction* findAgreedCallee(uint32_t position) const
    {
        const Vector<uint32_t, 1>& callees = resolvedAt(position).callees;
        const FlowSolvedConstruction& first = solvedOf(callees[0]);
        bool calleesAgree = callees.size() == 1 || std::ranges::all_of(callees, [&](uint32_t callee) {
            const FlowSolvedConstruction& solved = solvedOf(callee);
            return solved.stores == FlowConstructionStores::OnlyInitialOrder && solved.initialOrder == first.initialOrder;
        });
        return calleesAgree ? &first : nullptr;
    }

    FlowConstructionStores appendCreatedNames(uint32_t position, Vector<FlowConstructionName>& order)
    {
        auto addToOrder = [&](const FlowConstructionName& name) {
            if (m_acceptedNames.add(name))
                order.append(name);
        };
        if (resolvedAt(position).callees.isEmpty()) {
            addToOrder(nameAt(position));
            return FlowConstructionStores::OnlyInitialOrder;
        }
        const FlowSolvedConstruction* callee = findAgreedCallee(position);
        if (!callee)
            return FlowConstructionStores::Unknown;
        for (const FlowConstructionName& name : callee->initialOrder)
            addToOrder(name);
        return callee->stores;
    }

    const FlowConstructionGraph& m_graph;
    const FlowConstructionNode& m_node;
    const FlowConstructionSummary& m_summary;
    FunctionAnalyses& m_analyses;
    const Program& m_program;
    const CFG& m_cfg;
    struct CreatingBlock {
        BlockID block;
        uint32_t begin { 0 };
        uint32_t end { 0 };
    };

    Vector<uint32_t> m_creatingPoints;
    Vector<CreatingBlock> m_creatingBlocks;
    Vector<uint32_t> m_passedOverPoints;
    Vector<uint32_t> m_leakPoints;
    Vector<uint32_t> m_exitPoints;
    Vector<std::optional<BitVector>> m_reachableAfter;
    FlowConstructionNames m_acceptedNames;
};

} // anonymous namespace

void computeInitialOrder(FlowConstructionGraph& graph, uint32_t index)
{
    FlowConstructionNode& node = graph.nodes[index];
    if (!node.summary)
        return;
    InitialOrder order = InitialOrderBuilder(graph, node).build(node.solved.leak);
    node.solved.initialOrder = WTF::move(order.names);
    node.solved.stores = order.stores;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
