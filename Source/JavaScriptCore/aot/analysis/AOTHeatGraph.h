/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include <span>
#include <wtf/Forward.h>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class CallGraph;
class EffectSummaries;
class FlowResults;
class Program;

using HeatNodeID = ID<struct HeatNodeIDTag>;

namespace HeatModel {

inline constexpr double callsPerElement = 8;
inline constexpr double callsOfRepeatedCallback = 64;
inline constexpr double eventsPerRun = 32768;

} // namespace HeatModel

// Blocks of one function that run equally often in one activation.
struct HeatBlockGroup {
    double frequency { 0 };
    uint32_t size { 0 };
};

class HeatGraph {
public:
    using Node = HeatNodeID;

    HeatGraph() = default;

    unsigned numberOfFunctions() const { return m_sizes.size(); }

    Node root() const { return hostNode(); }
    unsigned numNodes() const { return numberOfFunctions() + 2; }
    unsigned index(Node node) const { return node.index(); }
    Node node(unsigned index) const { return m_isPresent[index] ? Node(index) : Node(); }
    std::span<const Node> successors(Node node) const LIFETIME_BOUND { return m_successors.span().subspan(m_firstSuccessor[node.index()], m_firstSuccessor[node.index() + 1] - m_firstSuccessor[node.index()]); }
    std::span<const Node> predecessors(Node node) const LIFETIME_BOUND { return m_predecessors.span().subspan(m_firstPredecessor[node.index()], m_firstPredecessor[node.index() + 1] - m_firstPredecessor[node.index()]); }
    double multiplierOf(Node from, unsigned successorIndex) const { return m_multipliers[m_firstSuccessor[from.index()] + successorIndex]; }

    Node hostNode() const { return Node(numberOfFunctions()); }
    Node unknownCodeNode() const { return Node(numberOfFunctions() + 1); }
    Node nodeOf(FunctionID function) const { return Node(function.index()); }
    FunctionID functionOf(Node node) const { return node.index() < numberOfFunctions() ? FunctionID(node.index()) : FunctionID(); }

    uint32_t sizeOf(FunctionID function) const { return m_sizes[function.index()]; }
    double massOf(FunctionID function) const { return m_masses[function.index()]; }
    std::span<const HeatBlockGroup> blockGroupsOf(FunctionID function) const LIFETIME_BOUND { return m_blockGroups[function.index()].span(); }

    void dump(PrintStream&) const;

private:
    friend HeatGraph buildHeatGraph(const Program&, const CallGraph&, const FlowResults&, const EffectSummaries&, BuildContext&);

    void computePredecessors();

    Vector<bool> m_isPresent;
    Vector<uint32_t> m_firstSuccessor;
    Vector<Node> m_successors;
    Vector<double> m_multipliers;
    Vector<uint32_t> m_firstPredecessor;
    Vector<Node> m_predecessors;
    Vector<uint32_t> m_sizes;
    Vector<double> m_masses;
    Vector<Vector<HeatBlockGroup, 4>> m_blockGroups;
};

HeatGraph buildHeatGraph(const Program&, const CallGraph&, const FlowResults&, const EffectSummaries&, BuildContext&);
Vector<double> estimateActivations(HeatGraph&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
