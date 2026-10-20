/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTDominators.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

namespace {

using SplitNodeID = ID<struct SplitNodeIDTag>;

class SplitNodeDump {
public:
    SplitNodeDump(BlockID block, bool isTerminator)
        : m_block(block)
        , m_isTerminator(isTerminator)
    {
    }

    void dump(PrintStream& out) const
    {
        out.print(BlockIDDump(m_block));
        if (m_isTerminator)
            out.print(".terminator");
    }

private:
    BlockID m_block;
    bool m_isTerminator;
};

class SplitBlockGraph {
    WTF_MAKE_NONCOPYABLE(SplitBlockGraph);
public:
    using Node = SplitNodeID;
    using Set = IndexSet<SplitNodeID>;
    template<typename T> using Map = IndexMap<SplitNodeID, T>;
    using List = Vector<SplitNodeID, 4>;

    explicit SplitBlockGraph(const CFG&);

    Node root() const { return headOf(m_cfg.root()); }
    unsigned numNodes() const { return m_blockOfNode.size(); }
    unsigned index(Node node) const { return node.index(); }
    Node node(unsigned index) const { return m_blockOfNode[index] ? Node(index) : Node(); }
    template<typename T> Map<T> newMap() const { return Map<T>(numNodes()); }

    std::span<const Node> successors(Node node) const { return entriesOf(node, m_successors, m_firstSuccessor); }
    std::span<const Node> predecessors(Node node) const { return entriesOf(node, m_predecessors, m_firstPredecessor); }

    Node headOf(BlockID block) const { return Node(block.index()); }
    Node terminatorOf(BlockID block) const { return m_terminatorOfBlock[block.index()]; }

    SplitNodeDump dump(Node) const;
    void dump(PrintStream&) const;

private:
    void createNodes();
    void appendSuccessorsOf(Node);
    void computeSuccessors();
    void computePredecessors();

    std::span<const Node> entriesOf(Node node, const Vector<Node>& entries, const Vector<uint32_t>& firstEntry) const
    {
        RELEASE_ASSERT(node.index() < numNodes());
        uint32_t begin = firstEntry[node.index()];
        return entries.subspan(begin, firstEntry[node.index() + 1] - begin);
    }

    const CFG& m_cfg;
    Vector<BlockID> m_blockOfNode;
    Vector<Node> m_terminatorOfBlock;
    Vector<Node> m_successors;
    Vector<uint32_t> m_firstSuccessor;
    Vector<Node> m_predecessors;
    Vector<uint32_t> m_firstPredecessor;
};

SplitBlockGraph::SplitBlockGraph(const CFG& cfg)
    : m_cfg(cfg)
{
    createNodes();
    computeSuccessors();
    computePredecessors();
}

void SplitBlockGraph::createNodes()
{
    unsigned numberOfBlocks = m_cfg.numNodes();
    m_blockOfNode.reserveInitialCapacity(numberOfBlocks);
    m_terminatorOfBlock.reserveInitialCapacity(numberOfBlocks);
    for (unsigned index = 0; index < numberOfBlocks; ++index) {
        m_blockOfNode.append(m_cfg.node(index));
        m_terminatorOfBlock.append(headOf(BlockID(index)));
    }
    for (unsigned index = 0; index < numberOfBlocks; ++index) {
        BlockID block = m_cfg.node(index);
        if (!block || !m_cfg.hasHandler(block))
            continue;
        m_terminatorOfBlock[index] = Node(m_blockOfNode.size());
        m_blockOfNode.append(block);
    }
}

void SplitBlockGraph::appendSuccessorsOf(Node node)
{
    BlockID block = m_blockOfNode[node.index()];
    if (!block)
        return;

    std::span<const BlockID> blockSuccessors = m_cfg.successors(block);
    if (!m_cfg.hasHandler(block)) {
        for (BlockID successor : blockSuccessors)
            m_successors.append(headOf(successor));
        return;
    }

    if (node == headOf(block)) {
        m_successors.append(terminatorOf(block));
        m_successors.append(headOf(blockSuccessors.back()));
        return;
    }

    for (BlockID successor : blockSuccessors.first(blockSuccessors.size() - 1))
        m_successors.append(headOf(successor));
}

void SplitBlockGraph::computeSuccessors()
{
    unsigned numberOfNodes = numNodes();
    m_firstSuccessor.reserveInitialCapacity(numberOfNodes + 1);
    m_firstSuccessor.append(0);
    for (unsigned index = 0; index < numberOfNodes; ++index) {
        appendSuccessorsOf(Node(index));
        m_firstSuccessor.append(m_successors.size());
    }
}

void SplitBlockGraph::computePredecessors()
{
    unsigned numberOfNodes = numNodes();
    m_firstPredecessor.fill(0, numberOfNodes + 1);
    for (Node successor : m_successors)
        ++m_firstPredecessor[successor.index() + 1];
    for (unsigned index = 0; index < numberOfNodes; ++index)
        m_firstPredecessor[index + 1] += m_firstPredecessor[index];

    m_predecessors.grow(m_successors.size());
    Vector<uint32_t> nextPredecessor = m_firstPredecessor;
    for (unsigned index = 0; index < numberOfNodes; ++index) {
        Node predecessor(index);
        for (Node successor : successors(predecessor))
            m_predecessors[nextPredecessor[successor.index()]++] = predecessor;
    }
}

SplitNodeDump SplitBlockGraph::dump(Node node) const
{
    if (!node)
        return SplitNodeDump(BlockID(), false);
    BlockID block = m_blockOfNode[node.index()];
    return SplitNodeDump(block, node != headOf(block));
}

void SplitBlockGraph::dump(PrintStream& out) const
{
    for (unsigned index = 0; index < numNodes(); ++index) {
        Node from = node(index);
        if (!from)
            continue;
        out.print(dump(from), " ->");
        for (Node successor : successors(from))
            out.print(" ", dump(successor));
        out.println();
    }
}

} // anonymous namespace

class Dominators::SplitBlockDominators {
    WTF_MAKE_NONCOPYABLE(SplitBlockDominators);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(SplitBlockDominators);
public:
    SplitBlockDominators(const CFG& cfg, bool selfCheck)
        : graph(cfg)
        , dominators(graph, selfCheck)
    {
    }

    SplitBlockGraph graph;
    WTF::Dominators<SplitBlockGraph> dominators;
};

Dominators::Dominators(CFG& cfg, bool selfCheck)
    : WTF::Dominators<CFG>(cfg, selfCheck)
    , m_cfg(cfg)
{
    computeChildren();
    computeJumpPointers();
    if (cfg.hasHandlers())
        m_splitBlockDominators = makeUnique<SplitBlockDominators>(cfg, selfCheck);
}

Dominators::~Dominators() = default;

void Dominators::computeChildren()
{
    unsigned numberOfBlocks = m_cfg.numNodes();
    auto dominatedBlocks = m_cfg.reversePostOrder().subspan(1);

    m_firstChild.fill(0, numberOfBlocks + 1);
    for (BlockID block : dominatedBlocks)
        ++m_firstChild[idom(block).index() + 1];
    for (unsigned index = 0; index < numberOfBlocks; ++index)
        m_firstChild[index + 1] += m_firstChild[index];

    m_children.grow(dominatedBlocks.size());
    Vector<uint32_t> nextChild = m_firstChild;
    for (BlockID block : dominatedBlocks)
        m_children[nextChild[idom(block).index()]++] = block;
}

void Dominators::computeJumpPointers()
{
    unsigned numberOfBlocks = m_cfg.numNodes();
    m_depth.fill(0, numberOfBlocks);
    m_jump.fill(m_cfg.root(), numberOfBlocks);
    for (BlockID block : m_cfg.reversePostOrder().subspan(1)) {
        BlockID dominator = idom(block);
        m_depth[block.index()] = depthOf(dominator) + 1;
        BlockID middle = jumpOf(dominator);
        m_jump[block.index()] = depthOf(dominator) - depthOf(middle) == depthOf(middle) - depthOf(jumpOf(middle)) ? jumpOf(middle) : dominator;
    }
}

BlockID Dominators::nearestCommonDominator(BlockID a, BlockID b) const
{
    RELEASE_ASSERT(m_cfg.isReachable(a) && m_cfg.isReachable(b));
    if (depthOf(a) < depthOf(b))
        std::swap(a, b);
    while (depthOf(a) > depthOf(b))
        a = depthOf(jumpOf(a)) >= depthOf(b) ? jumpOf(a) : idom(a);
    while (a != b) {
        bool canJump = jumpOf(a) != jumpOf(b);
        a = canJump ? jumpOf(a) : idom(a);
        b = canJump ? jumpOf(b) : idom(b);
    }
    return a;
}

bool Dominators::terminatorDominates(BlockID from, BlockID to) const
{
    if (!m_cfg.isReachable(from) || !m_cfg.isReachable(to))
        return false;
    if (from == to)
        return false;
    if (!m_cfg.hasHandler(from))
        return strictlyDominates(from, to);

    const SplitBlockGraph& graph = m_splitBlockDominators->graph;
    return m_splitBlockDominators->dominators.dominates(graph.terminatorOf(from), graph.headOf(to));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
