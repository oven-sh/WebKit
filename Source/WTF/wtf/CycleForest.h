/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <algorithm>
#include <climits>
#include <optional>
#include <span>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace WTF {

template<typename Graph>
class CycleForest {
    WTF_MAKE_NONCOPYABLE(CycleForest);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(CycleForest);
public:
    using Node = typename Graph::Node;

    static constexpr unsigned none = UINT_MAX;

    struct Cycle {
        Node header() const { return entries[0]; }
        bool isReducible() const { return entries.size() == 1; }

        Vector<Node, 2> entries;
        unsigned parent { none };
        unsigned depth { 1 };
    };

    template<typename ChooseHeader>
    CycleForest(Graph& graph, const ChooseHeader& chooseHeader)
        : m_graph(graph)
        , m_innermost(FillWith { }, graph.numNodes(), none)
    {
        Builder<ChooseHeader> builder(*this, chooseHeader);
        builder.run();
    }

    unsigned size() const { return m_cycles.size(); }
    const Cycle& operator[](unsigned index) const LIFETIME_BOUND { return m_cycles[index]; }
    unsigned innermost(Node node) const { return m_innermost[m_graph.index(node)]; }
    unsigned depth(unsigned cycle) const { return cycle == none ? 0 : m_cycles[cycle].depth; }
    bool isInCycle(Node node) const { return innermost(node) != none; }
    bool isHeader(Node node) const { return isInCycle(node) && m_cycles[innermost(node)].header() == node; }
    const Vector<Node>& nodesInPostOrder() const LIFETIME_BOUND { return m_postOrder; }

    unsigned nearestCommon(unsigned a, unsigned b) const
    {
        while (a != b) {
            if (a == none || b == none)
                return none;
            if (m_cycles[a].depth >= m_cycles[b].depth)
                a = m_cycles[a].parent;
            else
                b = m_cycles[b].parent;
        }
        return a;
    }

    bool contains(unsigned cycle, Node node) const { return nearestCommon(cycle, innermost(node)) == cycle; }

private:
    template<typename ChooseHeader>
    class Builder {
    public:
        Builder(CycleForest& forest, const ChooseHeader& chooseHeader)
            : m_forest(forest)
            , m_graph(forest.m_graph)
            , m_chooseHeader(chooseHeader)
            , m_number(FillWith { }, m_graph.numNodes(), 0u)
            , m_lowest(FillWith { }, m_graph.numNodes(), 0u)
            , m_isOnStack(FillWith { }, m_graph.numNodes(), false)
        {
        }

        void run()
        {
            Vector<Node> nodes;
            nodes.reserveInitialCapacity(m_graph.numNodes());
            for (unsigned index = 0; index < m_graph.numNodes(); ++index) {
                if (Node node = m_graph.node(index))
                    nodes.append(node);
            }
            decompose(nodes, none);
            while (!m_pending.isEmpty()) {
                Pending pending = m_pending.takeLast();
                decompose(pending.nodes, pending.cycle);
            }
            computePostOrder();
        }

    private:
        struct Frame {
            Node node;
            unsigned next;
        };

        struct Pending {
            unsigned cycle;
            Vector<Node> nodes;
        };

        unsigned indexOf(Node node) const { return m_graph.index(node); }
        bool isIn(unsigned cycle, Node node) const { return m_forest.m_innermost[indexOf(node)] == cycle; }

        bool hasSelfEdge(Node node) const
        {
            for (Node successor : m_graph.successors(node)) {
                if (successor == node)
                    return true;
            }
            return false;
        }

        void enter(Node node)
        {
            m_number[indexOf(node)] = m_lowest[indexOf(node)] = ++m_nextNumber;
            m_isOnStack[indexOf(node)] = true;
            m_stack.append(node);
            m_frames.append(Frame { node, 0 });
        }

        void decompose(const Vector<Node>& nodes, unsigned scope)
        {
            std::optional<Node> scopeHeader;
            if (scope != none)
                scopeHeader = m_forest.m_cycles[scope].header();
            for (Node node : nodes)
                m_number[indexOf(node)] = 0;
            for (Node start : nodes) {
                if (m_number[indexOf(start)])
                    continue;
                enter(start);
                while (!m_frames.isEmpty())
                    step(scope, scopeHeader);
            }
        }

        void step(unsigned scope, std::optional<Node> scopeHeader)
        {
            Node node = m_frames.last().node;
            auto&& successors = m_graph.successors(node);
            if (m_frames.last().next < successors.size()) {
                Node successor = successors[m_frames.last().next++];
                if (!isIn(scope, successor) || (scopeHeader && successor == *scopeHeader))
                    return;
                if (!m_number[indexOf(successor)])
                    enter(successor);
                else if (m_isOnStack[indexOf(successor)])
                    m_lowest[indexOf(node)] = std::min(m_lowest[indexOf(node)], m_number[indexOf(successor)]);
                return;
            }
            m_frames.removeLast();
            if (!m_frames.isEmpty()) {
                unsigned parent = indexOf(m_frames.last().node);
                m_lowest[parent] = std::min(m_lowest[parent], m_lowest[indexOf(node)]);
            }
            if (m_lowest[indexOf(node)] != m_number[indexOf(node)])
                return;
            unsigned first = m_stack.size() - 1;
            while (m_stack[first] != node)
                --first;
            std::span<const Node> members = m_stack.span().subspan(first);
            for (Node member : members)
                m_isOnStack[indexOf(member)] = false;
            if (members.size() > 1 || hasSelfEdge(node))
                addCycle(members, scope);
            m_stack.shrink(first);
        }

        void addCycle(std::span<const Node> members, unsigned parent)
        {
            unsigned cycle = m_forest.m_cycles.size();
            m_forest.m_cycles.append(Cycle { });
            m_forest.m_cycles[cycle].parent = parent;
            m_forest.m_cycles[cycle].depth = m_forest.depth(parent) + 1;
            for (Node member : members)
                m_forest.m_innermost[indexOf(member)] = cycle;

            Vector<Node, 2> entries;
            for (Node member : members) {
                for (Node predecessor : m_graph.predecessors(member)) {
                    if (!isIn(cycle, predecessor)) {
                        entries.append(member);
                        break;
                    }
                }
            }
            Node header = entries.size() == 1 ? entries[0] : m_chooseHeader(members, entries, [&](Node node) {
                return isIn(cycle, node);
            });
            auto& result = m_forest.m_cycles[cycle].entries;
            result.append(header);
            for (Node entry : entries) {
                if (entry != header)
                    result.append(entry);
            }
            if (members.size() == 1)
                return;
            Pending pending { cycle, { } };
            pending.nodes.reserveInitialCapacity(members.size() - 1);
            for (Node member : members) {
                if (member != header)
                    pending.nodes.append(member);
            }
            m_pending.append(WTF::move(pending));
        }

        std::optional<Node> targetInOrder(Node from, Node to) const
        {
            unsigned cycle = m_forest.innermost(to);
            unsigned common = m_forest.nearestCommon(m_forest.innermost(from), cycle);
            if (cycle == common) {
                if (m_forest.isHeader(to))
                    return std::nullopt;
                return to;
            }
            while (m_forest.m_cycles[cycle].parent != common)
                cycle = m_forest.m_cycles[cycle].parent;
            return m_forest.m_cycles[cycle].header();
        }

        void computePostOrder()
        {
            Vector<bool> isSeen(FillWith { }, m_graph.numNodes(), false);
            m_forest.m_postOrder.reserveInitialCapacity(m_graph.numNodes());
            Node root = m_graph.root();
            isSeen[indexOf(root)] = true;
            m_frames.append(Frame { root, 0 });
            while (!m_frames.isEmpty()) {
                Node node = m_frames.last().node;
                auto&& successors = m_graph.successors(node);
                if (m_frames.last().next < successors.size()) {
                    std::optional<Node> target = targetInOrder(node, successors[m_frames.last().next++]);
                    if (target && !isSeen[indexOf(*target)]) {
                        isSeen[indexOf(*target)] = true;
                        m_frames.append(Frame { *target, 0 });
                    }
                    continue;
                }
                m_forest.m_postOrder.append(node);
                m_frames.removeLast();
            }
        }

        CycleForest& m_forest;
        Graph& m_graph;
        const ChooseHeader& m_chooseHeader;
        Vector<unsigned> m_number;
        Vector<unsigned> m_lowest;
        Vector<bool> m_isOnStack;
        Vector<Node, 32> m_stack;
        Vector<Frame, 32> m_frames;
        Vector<Pending> m_pending;
        unsigned m_nextNumber { 0 };
    };

    Graph& m_graph;
    Vector<Cycle> m_cycles;
    Vector<unsigned> m_innermost;
    Vector<Node> m_postOrder;
};

} // namespace WTF
