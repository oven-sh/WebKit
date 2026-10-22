/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTDominators.h"

#if ENABLE(AOT)

#include <algorithm>

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
    : m_cfg(cfg)
    , m_selfCheck(selfCheck)
{
}

Dominators::~Dominators() = default;

auto Dominators::treeForChecking() const -> Tree&
{
    if (!m_treeForChecking)
        m_treeForChecking.emplace(m_cfg, m_selfCheck);
    return *m_treeForChecking;
}

bool Dominators::tryFindDominatorsByOrder(std::span<uint32_t> dominatorByOrder) const
{
    constexpr uint32_t unknown = UINT32_MAX;
    std::span<const BlockID> order = m_cfg.reversePostOrder();
    uint32_t size = order.size();
    const BlockID* successors = m_cfg.successorsOfAllBlocks().data();
    const uint32_t* firstSuccessor = m_cfg.firstSuccessorOfBlocks().data();
    uint32_t* dominatorOf = dominatorByOrder.data();
    std::ranges::fill(dominatorByOrder, unknown);
    dominatorOf[0] = 0;

    Vector<uint64_t, 64> isBelowDominatorOf(FillWith { }, size, 0ull);
    uint64_t stepsLeft = 16ull * (size + m_cfg.successorsOfAllBlocks().size()) + 64;
    uint64_t firstStampOfPass = 1;
    bool didChange;
    do {
        didChange = false;
        for (uint32_t source = 0; source < size; ++source) {
            unsigned block = order[source].index();
            for (uint32_t edge = firstSuccessor[block]; edge < firstSuccessor[block + 1]; ++edge) {
                uint32_t target = m_cfg.reversePostOrderIndex(successors[edge]);
                uint32_t known = dominatorOf[target];
                uint64_t stamp = firstStampOfPass + target;
                if (known == unknown) {
                    dominatorOf[target] = source;
                    isBelowDominatorOf[source] = stamp;
                    didChange = true;
                    continue;
                }
                uint32_t common = known;
                uint32_t above = source;
                while (above != common) {
                    if (above > common) {
                        if (isBelowDominatorOf[above] == stamp) {
                            above = common;
                            break;
                        }
                        isBelowDominatorOf[above] = stamp;
                        above = dominatorOf[above];
                    } else
                        common = dominatorOf[common];
                    if (!--stepsLeft) [[unlikely]]
                        return false;
                }
                if (common != known) {
                    dominatorOf[target] = common;
                    didChange = true;
                }
            }
        }
        firstStampOfPass += size;
    } while (didChange && m_cfg.hasCycle());
    return true;
}

void Dominators::computePlaces() const
{
    std::span<const BlockID> order = m_cfg.reversePostOrder();
    uint32_t size = order.size();
    m_places.fill(Place { }, m_cfg.numNodes());
    Place* places = m_places.mutableSpan().data();

    Vector<uint32_t, 192> tables(3 * size);
    std::span<uint32_t> dominatorOf = tables.mutableSpan().first(size);
    uint32_t* sizeOf = tables.mutableSpan().data() + size;
    uint32_t* nextNumberBelow = sizeOf + size;
    if (!tryFindDominatorsByOrder(dominatorOf)) [[unlikely]] {
        Tree& tree = treeForChecking();
        dominatorOf[0] = 0;
        for (uint32_t index = 1; index < size; ++index)
            dominatorOf[index] = m_cfg.reversePostOrderIndex(tree.idom(order[index]));
    }

    for (uint32_t index = 0; index < size; ++index)
        sizeOf[index] = 1;
    for (uint32_t index = size; --index;)
        sizeOf[dominatorOf[index]] += sizeOf[index];
    places[order[0].index()] = { BlockID(), 0, size - 1 };
    nextNumberBelow[0] = 1;
    for (uint32_t index = 1; index < size; ++index) {
        uint32_t dominator = dominatorOf[index];
        uint32_t number = nextNumberBelow[dominator];
        nextNumberBelow[dominator] += sizeOf[index];
        nextNumberBelow[index] = number + 1;
        places[order[index].index()] = { order[dominator], number, number + sizeOf[index] - 1 };
    }

    if (!m_selfCheck)
        return;
    Tree& tree = treeForChecking();
    for (unsigned index = 0; index < m_cfg.numNodes(); ++index)
        RELEASE_ASSERT(places[index].idom == tree.idom(BlockID(index)));
}

auto Dominators::splitBlockDominators() const -> const SplitBlockDominators&
{
    if (!m_splitBlockDominators)
        m_splitBlockDominators = makeUnique<SplitBlockDominators>(m_cfg, m_selfCheck);
    return *m_splitBlockDominators;
}

void Dominators::computeChildren() const
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

void Dominators::computeJumpPointers() const
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

BlockID Dominators::ancestorAtDepth(BlockID block, uint32_t depth) const
{
    while (depthOf(block) > depth)
        block = depthOf(jumpOf(block)) >= depth ? jumpOf(block) : idom(block);
    return block;
}

BlockID Dominators::nearestCommonDominator(BlockID a, BlockID b) const
{
    RELEASE_ASSERT(m_cfg.isReachable(a) && m_cfg.isReachable(b));
    if (a == b)
        return a;
    if (depthOf(a) < depthOf(b))
        std::swap(a, b);
    a = ancestorAtDepth(a, depthOf(b));
    while (a != b) {
        bool canJump = jumpOf(a) != jumpOf(b);
        a = canJump ? jumpOf(a) : idom(a);
        b = canJump ? jumpOf(b) : idom(b);
    }
    return a;
}

// A covered block may leave for its handler in front of its terminator, so the terminator dominates what the block dominates, less what the handler reaches without coming through the block again.
const BitVector& Dominators::blocksReachedFromHandlerOfDominator() const
{
    if (m_blocksReachedFromHandlerOfDominator)
        return *m_blocksReachedFromHandlerOfDominator;

    BitVector& isReached = m_blocksReachedFromHandlerOfDominator.emplace(m_cfg.numNodes());
    for (BlockID block : m_cfg.reversePostOrder()) {
        if (!m_cfg.hasHandler(block))
            continue;
        BlockID handler = m_cfg.successors(block).back();
        if (idom(handler) != block)
            continue;

        isReached.quickSet(handler.index());
        uint32_t depthOfChildren = depthOf(block) + 1;
        auto isEnteredFromReachedChild = [&](BlockID child) {
            return std::ranges::any_of(m_cfg.predecessors(child), [&](BlockID predecessor) {
                return predecessor != block && isReached.quickGet(ancestorAtDepth(predecessor, depthOfChildren).index());
            });
        };
        // The children stand in reverse post order, so only an edge of a cycle leads from a child to one in front of it.
        bool didReachChild;
        do {
            didReachChild = false;
            for (BlockID child : children(block)) {
                if (isReached.quickGet(child.index()) || !isEnteredFromReachedChild(child))
                    continue;
                isReached.quickSet(child.index());
                didReachChild = true;
            }
        } while (didReachChild && m_cfg.hasCycle());
    }
    return isReached;
}

bool Dominators::terminatorDominates(BlockID from, BlockID to) const
{
    if (!m_cfg.isReachable(from) || !m_cfg.isReachable(to))
        return false;
    if (from == to)
        return false;
    // A dominator comes before the blocks it dominates in reverse post order.
    if (m_cfg.reversePostOrderIndex(to) < m_cfg.reversePostOrderIndex(from))
        return false;
    if (!m_cfg.hasHandler(from))
        return strictlyDominates(from, to);

    return strictlyDominates(from, to) && !blocksReachedFromHandlerOfDominator().quickGet(ancestorAtDepth(to, depthOf(from) + 1).index());
}

bool Dominators::terminatorDominatesInSplitGraph(BlockID from, BlockID to) const
{
    if (from == to || !m_cfg.isReachable(from) || !m_cfg.isReachable(to))
        return false;
    const SplitBlockDominators& split = splitBlockDominators();
    return split.dominators.dominates(split.graph.terminatorOf(from), split.graph.headOf(to));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
