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
    CycleForest(Graph& graph, const ChooseHeader& chooseHeader, unsigned maxDepth = UINT_MAX)
        : m_graph(graph)
        , m_maxDepth(maxDepth)
        , m_innermost(FillWith { }, graph.numNodes(), none)
    {
        Builder<ChooseHeader> builder(*this, chooseHeader);
        builder.run();
    }

    struct Parts {
        Vector<Cycle> cycles;
        Vector<unsigned> innermost;
        Vector<Node> postOrder;
    };

    CycleForest(Graph& graph, Parts&& parts)
        : m_graph(graph)
        , m_maxDepth(UINT_MAX)
        , m_cycles(WTF::move(parts.cycles))
        , m_innermost(WTF::move(parts.innermost))
        , m_postOrder(WTF::move(parts.postOrder))
    {
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
        {
        }

        void run()
        {
            if (mayHaveCycle())
                findCycles();
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

        struct State {
            unsigned number { 0 };
            unsigned lowest { 0 };
            unsigned innermost { none };
            bool isOnStack { false };
        };

        bool mayHaveCycle() const
        {
            if constexpr (requires { m_graph.hasCycle(); })
                return m_graph.hasCycle();
            else
                return true;
        }

        void findCycles()
        {
            m_states.fill(State { }, m_graph.numNodes());
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
            for (unsigned index = 0; index < m_graph.numNodes(); ++index)
                m_forest.m_innermost[index] = m_states[index].innermost;
        }

        unsigned indexOf(Node node) const { return m_graph.index(node); }
        bool isIn(unsigned cycle, Node node) const { return m_states[indexOf(node)].innermost == cycle; }

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
            State& state = m_states[indexOf(node)];
            state.number = state.lowest = ++m_nextNumber;
            state.isOnStack = true;
            m_stack.append(node);
            m_frames.append(Frame { node, 0 });
        }

        void decompose(const Vector<Node>& nodes, unsigned scope)
        {
            std::optional<Node> scopeHeader;
            if (scope != none)
                scopeHeader = m_forest.m_cycles[scope].header();
            State* states = m_states.mutableSpan().data();
            for (Node node : nodes)
                states[indexOf(node)].number = 0;
            for (Node start : nodes) {
                if (states[indexOf(start)].number)
                    continue;
                enter(start);
                while (!m_frames.isEmpty())
                    walkOn(states, scope, scopeHeader);
            }
        }

        void walkOn(State* states, unsigned scope, std::optional<Node> scopeHeader)
        {
            Node node = m_frames.last().node;
            State& state = states[indexOf(node)];
            {
                auto&& successors = m_graph.successors(node);
                unsigned size = successors.size();
                unsigned lowest = state.lowest;
                for (unsigned next = m_frames.last().next; next < size;) {
                    Node successor = successors[next++];
                    const State& stateOfSuccessor = states[indexOf(successor)];
                    if (stateOfSuccessor.innermost != scope || (scopeHeader && successor == *scopeHeader))
                        continue;
                    if (!stateOfSuccessor.number) {
                        state.lowest = lowest;
                        m_frames.last().next = next;
                        enter(successor);
                        return;
                    }
                    if (stateOfSuccessor.isOnStack)
                        lowest = std::min(lowest, stateOfSuccessor.number);
                }
                state.lowest = lowest;
            }
            m_frames.removeLast();
            if (!m_frames.isEmpty()) {
                State& parent = states[indexOf(m_frames.last().node)];
                parent.lowest = std::min(parent.lowest, state.lowest);
            }
            if (state.lowest != state.number)
                return;
            unsigned first = m_stack.size() - 1;
            while (m_stack[first] != node)
                --first;
            std::span<const Node> members = m_stack.span().subspan(first);
            for (Node member : members)
                states[indexOf(member)].isOnStack = false;
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
                m_states[indexOf(member)].innermost = cycle;

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
            if (members.size() == 1 || m_forest.m_cycles[cycle].depth >= m_forest.m_maxDepth)
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
            if (!m_forest.size())
                return to;
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
                bool didEnter = false;
                {
                    auto&& successors = m_graph.successors(node);
                    unsigned size = successors.size();
                    for (unsigned next = m_frames.last().next; next < size;) {
                        std::optional<Node> target = targetInOrder(node, successors[next++]);
                        if (!target || isSeen[indexOf(*target)])
                            continue;
                        isSeen[indexOf(*target)] = true;
                        m_frames.last().next = next;
                        m_frames.append(Frame { *target, 0 });
                        didEnter = true;
                        break;
                    }
                }
                if (didEnter)
                    continue;
                m_forest.m_postOrder.append(node);
                m_frames.removeLast();
            }
        }

        CycleForest& m_forest;
        Graph& m_graph;
        const ChooseHeader& m_chooseHeader;
        Vector<State> m_states;
        Vector<Node, 32> m_stack;
        Vector<Frame, 32> m_frames;
        Vector<Pending> m_pending;
        unsigned m_nextNumber { 0 };
    };

    Graph& m_graph;
    unsigned m_maxDepth;
    Vector<Cycle> m_cycles;
    Vector<unsigned> m_innermost;
    Vector<Node> m_postOrder;
};

} // namespace WTF
