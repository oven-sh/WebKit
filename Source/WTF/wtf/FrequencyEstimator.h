/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <algorithm>
#include <wtf/CycleForest.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>

namespace WTF {

enum class EdgeClass : uint8_t { Normal, Unlikely, Rare };

struct FrequencyEstimatorConstants {
    double rareEdgeShare { 1.0 / 1024 };
    double unlikelyEdgeShare { 1.0 / 16 };
    double iterationsPerLoop { 10 };
    double minFrequency { 1e-6 };
    double maxFrequency { 1e6 };
    double returningNodeWeight { 0.25 };
    double maxUnlikelyEdgeShare { 0.5 };
    double maxMultiplier { 32768 };
    double maxMultipliedFrequency { 1e250 };
};

template<typename Graph>
class FrequencyPropagation {
    WTF_MAKE_NONCOPYABLE(FrequencyPropagation);
public:
    using Node = typename Graph::Node;
    using Cycles = CycleForest<Graph>;

    FrequencyPropagation(Graph& graph, const Cycles& cycles, const FrequencyEstimatorConstants& constants, double minFrequency, double maxFrequency)
        : m_graph(graph)
        , m_cycles(cycles)
        , m_iterations(FillWith { }, cycles.size(), constants.iterationsPerLoop)
        , m_returningShares(FillWith { }, cycles.size(), 1.0)
        , m_maxFlow(constants.maxMultipliedFrequency)
        , m_minFrequency(minFrequency)
        , m_maxFrequency(maxFrequency)
        , m_frequencies(FillWith { }, graph.numNodes(), 0.0)
    {
    }

    template<typename ForEachShare>
    void run(const ForEachShare& forEachShare)
    {
        Vector<double> incoming(FillWith { }, m_graph.numNodes(), 0.0);
        if (!m_cycles.size()) {
            incoming[m_graph.index(m_graph.root())] = 1;
            const Vector<Node>& postOrder = m_cycles.nodesInPostOrder();
            for (unsigned index = postOrder.size(); index--;) {
                Node node = postOrder[index];
                double flow = std::min(incoming[m_graph.index(node)], m_maxFlow);
                m_frequencies[m_graph.index(node)] = std::clamp(flow, m_minFrequency, m_maxFrequency);
                forEachShare(node, [&](Node successor, double share) {
                    incoming[m_graph.index(successor)] += flow * share;
                });
            }
            return;
        }
        Vector<double> flows(FillWith { }, m_graph.numNodes(), 0.0);
        Vector<double, 16> entering(FillWith { }, m_cycles.size(), 0.0);
        Node root = m_graph.root();
        incoming[m_graph.index(root)] = 1;
        for (unsigned cycle = m_cycles.innermost(root); cycle != Cycles::none; cycle = m_cycles[cycle].parent)
            entering[cycle] = 1;
        const Vector<Node>& postOrder = m_cycles.nodesInPostOrder();
        for (unsigned index = postOrder.size(); index--;) {
            Node node = postOrder[index];
            unsigned nodeCycle = m_cycles.innermost(node);
            double flow = std::min(m_cycles.isHeader(node) ? entering[nodeCycle] * m_iterations[nodeCycle] : incoming[m_graph.index(node)], m_maxFlow);
            m_frequencies[m_graph.index(node)] = std::clamp(flow, m_minFrequency, m_maxFrequency);
            flows[m_graph.index(node)] = flow;
            forEachShare(node, [&](Node successor, double share) {
                double amount = flow * share;
                unsigned cycle = m_cycles.innermost(successor);
                unsigned common = m_cycles.nearestCommon(nodeCycle, cycle);
                if (cycle == common) {
                    incoming[m_graph.index(successor)] += amount;
                    return;
                }
                for (; cycle != common; cycle = m_cycles[cycle].parent)
                    entering[cycle] += amount;
            });
        }
        // What a header gets from inside its cycle has come round.
        for (unsigned cycle = 0; cycle < m_cycles.size(); ++cycle) {
            unsigned header = m_graph.index(m_cycles[cycle].header());
            m_returningShares[cycle] = flows[header] > 0 ? incoming[header] / flows[header] : 1;
        }
    }

    double iterationsOf(unsigned cycle) const { return m_iterations[cycle]; }
    void setIterations(unsigned cycle, double iterations) { m_iterations[cycle] = iterations; }
    double returningShareOf(unsigned cycle) const { return m_returningShares[cycle]; }

    double frequencyOf(Node node) const { return m_frequencies[m_graph.index(node)]; }
    Vector<double> takeFrequencies() { return WTF::move(m_frequencies); }

private:
    Graph& m_graph;
    const Cycles& m_cycles;
    Vector<double, 16> m_iterations;
    Vector<double, 16> m_returningShares;
    double m_maxFlow;
    double m_minFrequency;
    double m_maxFrequency;
    Vector<double> m_frequencies;
};

template<typename Adaptor>
class FrequencyEstimator {
    WTF_MAKE_NONCOPYABLE(FrequencyEstimator);
public:
    using Graph = typename Adaptor::Graph;
    using Node = typename Adaptor::Node;
    using Cycles = CycleForest<Graph>;

    FrequencyEstimator(Adaptor& adaptor, const Cycles& cycles, const FrequencyEstimatorConstants& constants = { })
        : m_adaptor(adaptor)
        , m_graph(adaptor.graph())
        , m_cycles(cycles)
        , m_constants(constants)
        , m_propagation(m_graph, cycles, constants, constants.minFrequency, constants.maxFrequency)
        , m_weights(FillWith { }, m_graph.numNodes(), 1.0)
    {
        for (Node node : cycles.nodesInPostOrder())
            m_weights[m_graph.index(node)] = std::clamp(adaptor.initialWeight(node, constants), constants.rareEdgeShare, 1.0);
    }

    void run()
    {
        estimateWeights();
        estimateFrequencies();
    }

    // Behind this the weights and the classes of the edges are final. Only frequencyOf() needs estimateFrequencies(), which reads both and changes neither.
    void estimateWeights()
    {
        propagateWeightsBackward();
        markRareEdges();
        capWeights();
    }

    void estimateFrequencies() { propagateFrequencies(); }

    double frequencyOf(Node node) const { return m_propagation.frequencyOf(node); }
    double weightOf(Node node) const { return m_weights[m_graph.index(node)]; }

private:
    void lowerWeight(Node node, double weight)
    {
        double& current = m_weights[m_graph.index(node)];
        current = std::max(m_constants.rareEdgeShare, std::min(current, weight));
    }

    double edgeWeight(Node from, unsigned successorIndex) const
    {
        return edgeWeight(m_adaptor.successor(from, successorIndex), m_adaptor.edgeClass(from, successorIndex));
    }

    double edgeWeight(Node successor, EdgeClass edgeClass) const
    {
        double weight = weightOf(successor);
        switch (edgeClass) {
        case EdgeClass::Normal:
            return weight;
        case EdgeClass::Unlikely:
            return std::max(m_constants.rareEdgeShare, weight * m_constants.unlikelyEdgeShare);
        case EdgeClass::Rare:
            return m_constants.rareEdgeShare;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    bool isBackEdge(Node from, Node to) const { return m_cycles.isHeader(to) && m_cycles.contains(m_cycles.innermost(to), from); }

    void propagateWeightsBackward()
    {
        for (Node node : m_cycles.nodesInPostOrder()) {
            unsigned numberOfSuccessors = m_adaptor.numSuccessors(node);
            if (!numberOfSuccessors)
                continue;
            double likeliest = 0;
            for (unsigned successorIndex = 0; successorIndex < numberOfSuccessors; ++successorIndex)
                likeliest = std::max(likeliest, edgeWeight(node, successorIndex));
            lowerWeight(node, likeliest);
            m_adaptor.forEachDecidedPredecessor(node, [&](Node predecessor, unsigned successorIndex) {
                if (!isBackEdge(predecessor, node))
                    lowerWeight(predecessor, edgeWeight(node, successorIndex));
            });
        }
    }

    void markRareEdges()
    {
        for (Node node : m_cycles.nodesInPostOrder()) {
            if (weightOf(node) <= m_constants.rareEdgeShare)
                continue;
            unsigned numberOfSuccessors = m_adaptor.numSuccessors(node);
            for (unsigned successorIndex = 0; successorIndex < numberOfSuccessors; ++successorIndex) {
                Node successor = m_adaptor.successor(node, successorIndex);
                if (weightOf(successor) <= m_constants.rareEdgeShare && !isBackEdge(node, successor))
                    m_adaptor.setEdgeClass(node, successorIndex, EdgeClass::Rare);
            }
        }
    }

    void capWeights()
    {
        for (Node node : m_cycles.nodesInPostOrder())
            lowerWeight(node, m_adaptor.weightCapAfterPropagation(node, m_constants));
    }

    double leavingShare(Node from, Node to) const
    {
        if (!m_cycles.size())
            return 1;
        unsigned fromCycle = m_cycles.innermost(from);
        unsigned common = m_cycles.nearestCommon(fromCycle, m_cycles.innermost(to));
        double share = 1;
        for (unsigned cycle = fromCycle; cycle != common; cycle = m_cycles[cycle].parent)
            share /= m_propagation.iterationsOf(cycle);
        return share;
    }

    template<typename Send>
    void splitFlow(Node node, unsigned numberOfSuccessors, const Send& send) const
    {
        struct OutgoingEdge {
            Node successor;
            double share;
            double weight;
            bool isRare;
        };

        // As below, where one edge has all the weight that stays: a weight is above zero, and w / w is 1.
        if (numberOfSuccessors == 1) {
            Node successor = m_adaptor.successor(node, 0);
            double share = leavingShare(node, successor);
            if (m_adaptor.edgeClass(node, 0) == EdgeClass::Rare)
                share *= m_constants.rareEdgeShare;
            else if (share < 1)
                share *= share > m_constants.maxUnlikelyEdgeShare ? m_constants.maxUnlikelyEdgeShare / share : 1;
            else
                share = 1;
            send(successor, share);
            return;
        }

        Vector<OutgoingEdge, 4> edges;
        edges.reserveInitialCapacity(numberOfSuccessors);
        double stayingWeight = 0;
        double leaving = 0;
        double rare = 0;
        for (unsigned successorIndex = 0; successorIndex < numberOfSuccessors; ++successorIndex) {
            Node successor = m_adaptor.successor(node, successorIndex);
            EdgeClass edgeClass = m_adaptor.edgeClass(node, successorIndex);
            OutgoingEdge edge { successor, leavingShare(node, successor), 0, edgeClass == EdgeClass::Rare };
            if (edge.isRare)
                rare += edge.share * m_constants.rareEdgeShare;
            else if (edge.share < 1)
                leaving += edge.share;
            else {
                edge.weight = edgeWeight(successor, edgeClass);
                stayingWeight += edge.weight;
            }
            edges.append(edge);
        }
        double leavingScale = leaving > m_constants.maxUnlikelyEdgeShare ? m_constants.maxUnlikelyEdgeShare / leaving : 1;
        rare = std::min(rare, m_constants.maxUnlikelyEdgeShare);
        for (const OutgoingEdge& edge : edges) {
            double share = edge.share;
            if (edge.isRare)
                share *= m_constants.rareEdgeShare;
            else if (share < 1)
                share *= leavingScale * (1 - rare);
            else
                share = (1 - rare) * (1 - leaving * leavingScale) * edge.weight / stayingWeight;
            send(edge.successor, share);
        }
    }

    // A cycle that flow seldom goes round is no loop: a hot loop and its rare copy that lead into each other, a step that is tried again where it failed.
    void propagateFrequencies()
    {
        propagateFrequenciesOnce();
        unsigned deepest = 0;
        for (unsigned cycle = 0; cycle < m_cycles.size(); ++cycle)
            deepest = std::max(deepest, m_cycles.depth(cycle));
        // From the inside out: what comes back to the header of a loop has passed through the cycles inside it, and a factor of ten that is wrong there lets little of it out again.
        for (unsigned depth = deepest; depth; --depth) {
            bool hasCycleSeldomGoneRound = false;
            for (unsigned cycle = 0; cycle < m_cycles.size(); ++cycle) {
                double returning = m_propagation.returningShareOf(cycle);
                if (m_cycles.depth(cycle) != depth || returning > m_constants.unlikelyEdgeShare)
                    continue;
                m_propagation.setIterations(cycle, 1 / (1 - returning));
                hasCycleSeldomGoneRound = true;
            }
            if (hasCycleSeldomGoneRound)
                propagateFrequenciesOnce();
        }
    }

    void propagateFrequenciesOnce()
    {
        m_propagation.run([&](Node node, const auto& send) {
            unsigned numberOfSuccessors = m_adaptor.numSuccessors(node);
            if (!m_adaptor.entersEverySuccessor(node)) {
                splitFlow(node, numberOfSuccessors, send);
                return;
            }
            for (unsigned successorIndex = 0; successorIndex < numberOfSuccessors; ++successorIndex)
                send(m_adaptor.successor(node, successorIndex), successorIndex ? m_constants.rareEdgeShare : 1.0);
        });
    }

    Adaptor& m_adaptor;
    Graph& m_graph;
    const Cycles& m_cycles;
    const FrequencyEstimatorConstants m_constants;
    FrequencyPropagation<Graph> m_propagation;
    Vector<double> m_weights;
};

template<typename Graph, typename MultiplierOf>
Vector<double> estimateMultipliedFrequencies(Graph& graph, const CycleForest<Graph>& cycles, const MultiplierOf& multiplierOf, const FrequencyEstimatorConstants& constants = { })
{
    FrequencyPropagation<Graph> propagation(graph, cycles, constants, 0, constants.maxMultipliedFrequency);
    propagation.run([&](typename Graph::Node node, const auto& send) {
        auto&& successors = graph.successors(node);
        for (unsigned successorIndex = 0; successorIndex < successors.size(); ++successorIndex)
            send(successors[successorIndex], std::clamp(multiplierOf(node, successorIndex), 0.0, constants.maxMultiplier));
    });
    return propagation.takeFrequencies();
}

} // namespace WTF
