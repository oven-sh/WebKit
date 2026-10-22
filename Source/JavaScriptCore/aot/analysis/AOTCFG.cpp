/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTCFG.h"

#if ENABLE(AOT)

#include <algorithm>
#include <wtf/PrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

Edge outgoingEdgeAt(const Function& function, BlockID id, uint32_t successorIndex)
{
    const Block& block = function.at(id);
    if (successorIndex < block.terminator.targets.size())
        return Edge { id, successorIndex };
    RELEASE_ASSERT(successorIndex == block.terminator.targets.size() && block.handler);
    return Edge::toHandlerOf(id);
}

uint32_t indexAfterCaughtValues(const Function& function, const Block& block)
{
    uint32_t index = 0;
    for (ValueID id : block.values) {
        Opcode opcode = function.at(id).opcode;
        if (opcode != Opcode::CatchValue && opcode != Opcode::CatchException)
            break;
        ++index;
    }
    return index;
}

bool returnsToCaller(TerminatorKind kind)
{
    switch (kind) {
    case TerminatorKind::Return:
    case TerminatorKind::ReturnCall:
        return true;
    case TerminatorKind::Jump:
    case TerminatorKind::Branch:
    case TerminatorKind::Switch:
    case TerminatorKind::SwitchString:
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
    case TerminatorKind::Unreachable:
    case TerminatorKind::SwitchOnShape:
    case TerminatorKind::TryCallInline:
    case TerminatorKind::TestAndBranch:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

IncomingValue incomingValueOf(const Function& function, Edge edge, uint32_t parameterIndex)
{
    const Target& target = targetOf(function, edge);
    RELEASE_ASSERT(parameterIndex < function.at(target.block).parameters.size());
    if (parameterIndex < target.arguments.size())
        return { IncomingValue::Kind::Argument, target.arguments[parameterIndex], { } };
    RELEASE_ASSERT(!edge.isHandlerEdge());
    uint32_t resultIndex = parameterIndex - static_cast<uint32_t>(target.arguments.size());
    RELEASE_ASSERT(resultIndex < numberOfEdgeResults(function, edge.from, edge.targetIndex));
    return { IncomingValue::Kind::EdgeResult, ValueID(), { edge.from, edge.targetIndex, resultIndex } };
}

ValueID valuePassedOn(const Function& function, Edge edge, uint32_t parameterIndex)
{
    const Target& target = targetOf(function, edge);
    RELEASE_ASSERT(parameterIndex < function.at(target.block).parameters.size());
    if (parameterIndex < target.arguments.size())
        return target.arguments[parameterIndex];
    RELEASE_ASSERT(!edge.isHandlerEdge());
    return operandNarrowedOnEdge(function, { edge.from, edge.targetIndex, parameterIndex - static_cast<uint32_t>(target.arguments.size()) });
}

uint32_t numberOfExplicitArguments(const Function& function, Edge edge)
{
    return static_cast<uint32_t>(targetOf(function, edge).arguments.size());
}

uint32_t numberOfSharedExplicitParameters(const Function& function, const CFG& cfg, BlockID block)
{
    uint32_t largest = 0;
    for (Edge edge : cfg.incomingEdges(block)) {
        if (!edge.isHandlerEdge())
            largest = std::max(largest, numberOfEdgeResults(function, edge.from, edge.targetIndex));
    }
    uint32_t numberOfParameters = static_cast<uint32_t>(function.at(block).parameters.size());
    RELEASE_ASSERT(largest <= numberOfParameters);
    return numberOfParameters - largest;
}

void BlockIDDump::dump(PrintStream& out) const
{
    if (m_block)
        out.print("b", m_block.index());
    else
        out.print("-");
}

static_assert(sizeof(BlockID) == sizeof(uint32_t) && sizeof(Edge) == 3 * sizeof(uint32_t) && alignof(Edge) == alignof(uint32_t));

CFG::CFG(const Function& function)
    : m_numberOfBlocks(function.blocks.size())
{
    RELEASE_ASSERT(function.hasBody());
    readSuccessors(function);
    computeReversePostOrder();
}

bool CFG::describes(const Function& function) const
{
    if (function.blocks.size() != m_numberOfBlocks)
        return false;
    for (BlockID id : m_reversePostOrder) {
        const Block& block = function.at(id);
        std::span<const BlockID> knownSuccessors = successors(id);
        if (hasHandler(id) != !!block.handler || knownSuccessors.size() != numberOfOutgoingEdges(block))
            return false;
        size_t successorIndex = 0;
        bool hasKnownSuccessors = true;
        block.forEachTarget([&](const Target& target) {
            hasKnownSuccessors &= target.block == knownSuccessors[successorIndex++];
        });
        if (!hasKnownSuccessors)
            return false;
    }
    return true;
}

void CFG::readSuccessors(const Function& function)
{
    unsigned numberOfBlocks = m_numberOfBlocks;
    size_t sizeOfTablesByBlock = 3 * static_cast<size_t>(numberOfBlocks) + 1;
    size_t capacity = 2 * static_cast<size_t>(numberOfBlocks) + 6;
    m_tables.grow(sizeOfTablesByBlock + capacity);
    uint32_t* firstSuccessor = m_tables.mutableSpan().data();
    uint32_t* successors = firstSuccessor + sizeOfTablesByBlock;
    size_t end = 0;
    unsigned index = 0;
    for (const Block& block : function.blocks) {
        firstSuccessor[index] = static_cast<uint32_t>(end);
        std::span<const Target> targets = block.terminator.targets.span();
        if (end + targets.size() + 1 > capacity) [[unlikely]] {
            capacity = 2 * (end + targets.size() + 1);
            m_tables.grow(sizeOfTablesByBlock + capacity);
            firstSuccessor = m_tables.mutableSpan().data();
            successors = firstSuccessor + sizeOfTablesByBlock;
        }
        for (const Target& target : targets) {
            RELEASE_ASSERT(target.block.index() < numberOfBlocks);
            successors[end++] = target.block.index();
        }
        if (block.handler) [[unlikely]] {
            RELEASE_ASSERT(block.handler->block.index() < numberOfBlocks);
            successors[end++] = block.handler->block.index();
            m_hasHandler.set(index);
        }
        ++index;
    }
    RELEASE_ASSERT(end < beingWalked);
    firstSuccessor[numberOfBlocks] = static_cast<uint32_t>(end);
    m_firstSuccessor = firstSuccessor;
    m_reversePostOrderIndex = firstSuccessor + numberOfBlocks + 1;
    m_successors = reinterpret_cast<const BlockID*>(successors);
}

void CFG::computeReversePostOrder()
{
    struct Frame {
        uint32_t block;
        uint32_t endOfUnvisitedEdges;
    };

    unsigned numberOfBlocks = m_numberOfBlocks;
    const uint32_t* firstSuccessor = m_firstSuccessor;
    const uint32_t* successors = reinterpret_cast<const uint32_t*>(m_successors);
    uint32_t* orderIndex = const_cast<uint32_t*>(m_reversePostOrderIndex);
    uint32_t* order = orderIndex + numberOfBlocks;
    std::fill_n(orderIndex, numberOfBlocks, notReached);

    // A block is on the stack at most once.
    Vector<Frame, 64> stackStorage(numberOfBlocks);
    Frame* stack = stackStorage.mutableSpan().data();
    size_t depth = 1;
    stack[0] = { 0, firstSuccessor[1] };
    orderIndex[0] = beingWalked;

    bool hasCycle = false;
    uint32_t beginOfOrder = numberOfBlocks;
    while (depth) {
        Frame& frame = stack[depth - 1];
        uint32_t block = frame.block;
        if (frame.endOfUnvisitedEdges == firstSuccessor[block]) {
            order[--beginOfOrder] = block;
            orderIndex[block] = beginOfOrder;
            --depth;
            continue;
        }
        uint32_t target = successors[--frame.endOfUnvisitedEdges];
        uint32_t state = orderIndex[target];
        if (state == notReached) {
            orderIndex[target] = beingWalked;
            stack[depth++] = { target, firstSuccessor[target + 1] };
            continue;
        }
        RELEASE_ASSERT(target);
        hasCycle |= state == beingWalked;
    }
    m_hasCycle = hasCycle;
    m_reversePostOrder = { reinterpret_cast<const BlockID*>(order) + beginOfOrder, numberOfBlocks - beginOfOrder };
    if (beginOfOrder) [[unlikely]]
        forgetUnreachableBlocks(beginOfOrder);
}

void CFG::forgetUnreachableBlocks(uint32_t numberOfUnreachableBlocks)
{
    unsigned numberOfBlocks = m_numberOfBlocks;
    uint32_t* firstSuccessor = const_cast<uint32_t*>(m_firstSuccessor);
    uint32_t* successors = reinterpret_cast<uint32_t*>(const_cast<BlockID*>(m_successors));
    uint32_t* orderIndex = const_cast<uint32_t*>(m_reversePostOrderIndex);
    for (BlockID block : m_reversePostOrder)
        orderIndex[block.index()] -= numberOfUnreachableBlocks;

    unsigned block = 0;
    while (block < numberOfBlocks && (orderIndex[block] != notReached || firstSuccessor[block] == firstSuccessor[block + 1]))
        ++block;
    if (block == numberOfBlocks)
        return;

    uint32_t end = firstSuccessor[block];
    for (; block < numberOfBlocks; ++block) {
        uint32_t first = firstSuccessor[block];
        uint32_t endOfBlock = firstSuccessor[block + 1];
        firstSuccessor[block] = end;
        if (orderIndex[block] == notReached) {
            if (first != endOfBlock)
                m_hasHandler.clear(block);
            continue;
        }
        for (; first < endOfBlock; ++first)
            successors[end++] = successors[first];
    }
    firstSuccessor[numberOfBlocks] = end;
}

void CFG::computeIncomingEdges() const
{
    unsigned numberOfBlocks = m_numberOfBlocks;
    const uint32_t* firstSuccessor = m_firstSuccessor;
    const uint32_t* successors = reinterpret_cast<const uint32_t*>(m_successors);
    uint32_t numberOfEdges = firstSuccessor[numberOfBlocks];

    // While the edges are filed, the entry behind a block's own says where its next edge goes. At the end that is where the edges of the next block begin.
    size_t sizeOfTableByBlock = static_cast<size_t>(numberOfBlocks) + 2;
    m_incomingTables.grow(sizeOfTableByBlock + 3 * static_cast<size_t>(numberOfEdges));
    uint32_t* firstIncomingEdge = m_incomingTables.mutableSpan().data();
    Edge* incomingEdges = reinterpret_cast<Edge*>(firstIncomingEdge + sizeOfTableByBlock);
    std::fill_n(firstIncomingEdge, sizeOfTableByBlock, 0);
    uint32_t* nextIncomingEdge = firstIncomingEdge + 1;

    for (uint32_t edge = 0; edge < numberOfEdges; ++edge)
        ++nextIncomingEdge[successors[edge] + 1];
    for (unsigned block = 0; block < numberOfBlocks; ++block)
        nextIncomingEdge[block + 1] += nextIncomingEdge[block];

    bool hasHandlers = this->hasHandlers();
    for (unsigned block = 0; block < numberOfBlocks; ++block) {
        uint32_t first = firstSuccessor[block];
        uint32_t end = firstSuccessor[block + 1];
        if (first == end)
            continue;
        bool hasHandler = hasHandlers && m_hasHandler.get(block);
        uint32_t endOfTargets = end - (hasHandler ? 1 : 0);
        for (uint32_t edge = first; edge < endOfTargets; ++edge)
            new (NotNull, &incomingEdges[nextIncomingEdge[successors[edge]]++]) Edge { BlockID(block), edge - first };
        if (hasHandler) [[unlikely]]
            new (NotNull, &incomingEdges[nextIncomingEdge[successors[endOfTargets]]++]) Edge(Edge::toHandlerOf(BlockID(block)));
    }
    m_incomingEdges = incomingEdges;
    m_firstIncomingEdge = firstIncomingEdge;
}

void CFG::findLargestNumberOfSuccessors() const
{
    uint32_t largest = 0;
    for (unsigned block = 0; block < m_numberOfBlocks; ++block)
        largest = std::max(largest, m_firstSuccessor[block + 1] - m_firstSuccessor[block]);
    m_largestNumberOfSuccessors = largest;
}

void CFG::computePredecessors() const
{
    if (!m_firstIncomingEdge)
        computeIncomingEdges();
    unsigned numberOfBlocks = m_numberOfBlocks;
    const uint32_t* firstIncomingEdge = m_firstIncomingEdge;
    const Edge* incomingEdges = m_incomingEdges;
    m_firstPredecessor.grow(numberOfBlocks + 1);
    m_predecessors.grow(firstIncomingEdge[numberOfBlocks]);
    uint32_t* firstPredecessor = m_firstPredecessor.mutableSpan().data();
    BlockID* predecessors = m_predecessors.mutableSpan().data();
    uint32_t size = 0;
    for (unsigned block = 0; block < numberOfBlocks; ++block) {
        firstPredecessor[block] = size;
        BlockID previous;
        for (uint32_t edge = firstIncomingEdge[block]; edge < firstIncomingEdge[block + 1]; ++edge) {
            BlockID from = incomingEdges[edge].from;
            if (from != previous)
                predecessors[size++] = from;
            previous = from;
        }
    }
    firstPredecessor[numberOfBlocks] = size;
    m_predecessors.shrink(size);
}

std::optional<Edge> CFG::singleIncomingEdge(BlockID block) const
{
    auto edges = incomingEdges(block);
    if (edges.size() != 1)
        return std::nullopt;
    return edges[0];
}

BitVector CFG::blocksReachableFrom(BlockID start, const BitVector& barrier) const
{
    RELEASE_ASSERT(start.index() < m_numberOfBlocks);
    BitVector result(m_numberOfBlocks);
    result.set(start.index());
    Vector<unsigned, 16> worklist { start.index() };
    while (!worklist.isEmpty()) {
        for (BlockID target : successors(BlockID(worklist.takeLast()))) {
            unsigned block = target.index();
            if (!barrier.get(block) && !result.set(block))
                worklist.append(block);
        }
    }
    return result;
}

bool CFG::isInCycleAvoiding(BlockID block, BlockID avoidedBlock) const
{
    RELEASE_ASSERT(block.index() < m_numberOfBlocks && avoidedBlock.index() < m_numberOfBlocks);
    if (block == avoidedBlock)
        return false;
    BitVector isVisited(m_numberOfBlocks);
    isVisited.set(avoidedBlock.index());
    Vector<unsigned, 16> worklist { block.index() };
    while (!worklist.isEmpty()) {
        for (BlockID successor : successors(BlockID(worklist.takeLast()))) {
            if (successor == block)
                return true;
            if (!isVisited.set(successor.index()))
                worklist.append(successor.index());
        }
    }
    return false;
}

void CFG::dump(PrintStream& out) const
{
    for (BlockID block : m_reversePostOrder) {
        out.print(dump(block), " ->");
        for (BlockID successor : successors(block))
            out.print(" ", dump(successor));
        out.println();
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
