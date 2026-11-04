/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <memory>
#include <wtf/IndexMap.h>
#include <wtf/IndexSet.h>
#include <wtf/Noncopyable.h>
#include <wtf/PrintStream.h>
#include <wtf/Vector.h>
#include <wtf/WeakRandom.h>

namespace TestWebKitAPI {

class TestBasicBlock {
public:
    explicit TestBasicBlock(unsigned index)
        : m_index(index)
    {
    }

    unsigned index() const { return m_index; }

    void dump(PrintStream& out) const
    {
        out.print("#"_s, m_index);
    }

    Vector<TestBasicBlock*, 2> successors;
    Vector<TestBasicBlock*, 2> predecessors;

private:
    unsigned m_index;
};

class TestControlFlowGraph {
    WTF_MAKE_NONCOPYABLE(TestControlFlowGraph);
public:
    using Node = TestBasicBlock*;
    using Set = IndexSet<TestBasicBlock*>;
    template<typename T> using Map = IndexMap<TestBasicBlock*, T>;
    using List = Vector<TestBasicBlock*, 4>;

    explicit TestControlFlowGraph(unsigned numberOfBlocks, unsigned numberOfNullNodes = 0)
        : m_blocks(numberOfBlocks, [](size_t index) {
            return makeUniqueWithoutFastMallocCheck<TestBasicBlock>(static_cast<unsigned>(index));
        })
        , m_numberOfNullNodes(numberOfNullNodes)
    {
        ASSERT(numberOfBlocks);
    }

    void addEdge(unsigned from, unsigned to)
    {
        ASSERT(to);
        node(from)->successors.append(node(to));
        node(to)->predecessors.append(node(from));
    }

    void removeBlock(unsigned index)
    {
        ASSERT(index);
        ASSERT(node(index)->successors.isEmpty() && node(index)->predecessors.isEmpty());
        m_blocks[index] = nullptr;
    }

    void addRandomEdges(WeakRandom& random, unsigned numberOfExtraEdges)
    {
        unsigned numberOfBlocks = m_blocks.size();
        ASSERT(numberOfBlocks >= 2);
        for (unsigned index = 1; index < numberOfBlocks; ++index) {
            if (random.getUint32(8))
                addEdge(random.getUint32(index), index);
        }
        while (numberOfExtraEdges--) {
            unsigned from = random.getUint32(numberOfBlocks);
            unsigned to = 1 + random.getUint32(numberOfBlocks - 1);
            addEdge(from, to);
        }
    }

    Node root() { return node(0); }

    template<typename T> Map<T> newMap() { return Map<T>(numNodes()); }

    Vector<TestBasicBlock*, 2>& successors(Node node) { return node->successors; }
    Vector<TestBasicBlock*, 2>& predecessors(Node node) { return node->predecessors; }

    unsigned index(Node node) const { return node->index(); }
    Node node(unsigned index) const { return index < m_blocks.size() ? m_blocks[index].get() : nullptr; }
    unsigned numNodes() const { return m_blocks.size() + m_numberOfNullNodes; }

    PointerDump<TestBasicBlock> dump(Node node) const { return pointerDump(node); }

    void dump(PrintStream& out) const
    {
        for (auto& block : m_blocks) {
            if (!block)
                continue;
            out.print(*block, ":"_s);
            for (TestBasicBlock* successor : block->successors)
                out.print(" "_s, *successor);
            out.print("\n"_s);
        }
    }

private:
    Vector<std::unique_ptr<TestBasicBlock>> m_blocks;
    unsigned m_numberOfNullNodes;
};

} // namespace TestWebKitAPI
