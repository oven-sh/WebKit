/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTThreads.h"
#include "AOTThreshold.h"
#include "AOTVector.h"
#include <span>
#include <wtf/FastMalloc.h>
#include <wtf/Forward.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class BodyReferences;
class CallGraph;
class EffectSummaries;
class FlowResults;
class Program;

using HeatNodeID = ID<struct HeatNodeIDTag>;

namespace HeatModel {

inline constexpr double callsPerElement = 8;
inline constexpr double callsOfRepeatedCallback = 64;
inline constexpr double eventsPerRun = 32768;
// So often a loop in a loop enters what it calls.
AOT_THRESHOLD(double, minEntriesPerEventOfHotCode, 100);
AOT_THRESHOLD(unsigned, warmCodeLimitsInVeryLargeProgram, 4);

} // namespace HeatModel

struct HeatBlockGroup {
    double frequency { 0 };
    uint32_t size { 0 };
};

struct WeightOfBlock {
    BlockID block;
    double weight { 0 };
    double share { 0 };

    friend bool operator==(const WeightOfBlock&, const WeightOfBlock&) = default;
};

class HeatGraph;

class BodyWeights {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(BodyWeights);
    WTF_MAKE_NONCOPYABLE(BodyWeights);
public:
    BodyWeights() = default;

    void didChange(std::span<const FunctionID>);
    bool keepsSomethingOf(FunctionID id) const { return id.index() < m_blocks.size() && !m_blocks[id.index()].isEmpty(); }

private:
    friend HeatGraph buildHeatGraph(const Program&, const CallGraph&, const FlowResults&, const EffectSummaries&, BuildContext&, const BodyReferences*, const BodyWeights*, BodyWeights*, HeatGraph*);

    Vector<Vector<WeightOfBlock>> m_blocks;
};

class HeatGraph {
public:
    using Node = HeatNodeID;

    unsigned numberOfFunctions() const { return m_sizes.size(); }

    Node root() const { return hostNode(); }
    unsigned numNodes() const { return numberOfFunctions() + 2 + m_numberOfNames; }
    unsigned index(Node node) const { return node.index(); }
    Node node(unsigned index) const { return m_isPresent[index] ? Node(index) : Node(); }
    std::span<const Node> successors(Node node) const LIFETIME_BOUND { return m_successors.span().subspan(m_firstSuccessor[node.index()], m_numberOfSuccessors[node.index()]); }
    std::span<const Node> predecessors(Node node) const LIFETIME_BOUND { return m_predecessors.span().subspan(m_firstPredecessor[node.index()], m_firstPredecessor[node.index() + 1] - m_firstPredecessor[node.index()]); }
    double multiplierOf(Node from, unsigned successorIndex) const { return m_multipliers[m_firstSuccessor[from.index()] + successorIndex]; }

    Node hostNode() const { return Node(numberOfFunctions()); }
    Node unknownCodeNode() const { return Node(numberOfFunctions() + 1); }
    double entriesPerEventOf(FunctionID function) const { return function.index() < m_entriesPerEvent.size() ? m_entriesPerEvent[function.index()] : 0; }
    Node nodeOfName(size_t position) const { return Node(numberOfFunctions() + 2 + position); }
    Node nodeOf(FunctionID function) const { return Node(function.index()); }
    FunctionID functionOf(Node node) const { return node.index() < numberOfFunctions() ? FunctionID(node.index()) : FunctionID(); }

    uint32_t sizeOf(FunctionID function) const { return m_sizes[function.index()]; }
    double massOf(FunctionID function) const { return m_masses[function.index()]; }
    std::span<const HeatBlockGroup> blockGroupsOf(FunctionID function) const LIFETIME_BOUND { return m_blockGroups[function.index()].span(); }

    void dump(PrintStream&) const;

private:
    friend HeatGraph buildHeatGraph(const Program&, const CallGraph&, const FlowResults&, const EffectSummaries&, BuildContext&, const BodyReferences*, const BodyWeights*, BodyWeights*, HeatGraph*);

    unsigned m_numberOfNames { 0 };
    Vector<double> m_entriesPerEvent;
    Vector<bool> m_isPresent;
    // Nodes with equal lists of successors and multipliers share one range.
    Vector<uint32_t> m_firstSuccessor;
    Vector<uint32_t> m_numberOfSuccessors;
    Vector<Node> m_successors;
    Vector<double> m_multipliers;
    Vector<uint32_t> m_firstPredecessor;
    Vector<Node> m_predecessors;
    Vector<uint32_t> m_sizes;
    Vector<double> m_masses;
    ParallelArray<Vector<HeatBlockGroup, 4>> m_blockGroups;
};

HeatGraph buildHeatGraph(const Program&, const CallGraph&, const FlowResults&, const EffectSummaries&, BuildContext&, const BodyReferences* = nullptr, const BodyWeights* kept = nullptr, BodyWeights* made = nullptr, HeatGraph* graphOfClasses = nullptr);
Vector<double> estimateActivations(HeatGraph&);
// Most of a very large program never runs, and which part does is not in an estimate: its classes are read off the calls that are written in it.
uint64_t maxValuesOfOrdinaryProgram(const BuildContext&);
bool isVeryLargeProgram(const Program&, const BuildContext&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
