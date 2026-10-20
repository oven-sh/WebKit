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

template<typename FunctionType>
static auto& findTarget(FunctionType& function, Edge edge)
{
    auto& block = function.at(edge.from);
    switch (edge.kind) {
    case EdgeKind::Normal:
        return block.terminator.targets[edge.targetIndex];
    case EdgeKind::Handler:
        RELEASE_ASSERT(block.handler);
        return *block.handler;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

const Target& targetOf(const Function& function, Edge edge)
{
    return findTarget(function, edge);
}

Target& targetOf(Function& function, Edge edge)
{
    return findTarget(function, edge);
}

ProgramPoint pointBeforeTerminatorOf(const Function& function, BlockID block)
{
    return { block, static_cast<uint32_t>(function.at(block).values.size()) };
}

uint32_t numberOfOutgoingEdges(const Block& block)
{
    return static_cast<uint32_t>(block.terminator.targets.size()) + (block.handler ? 1 : 0);
}

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
    IncomingValue incoming = incomingValueOf(function, edge, parameterIndex);
    switch (incoming.kind) {
    case IncomingValue::Kind::Argument:
        return incoming.argument;
    case IncomingValue::Kind::EdgeResult:
        break;
    }

    EdgeResult result = edgeResultAt(function, incoming.result);
    switch (result.kind) {
    case EdgeResultKind::NarrowedOperand:
        return result.subject;
    case EdgeResultKind::Guard:
    case EdgeResultKind::DeclaredResult:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
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

CFG::CFG(const Function& function)
    : m_numberOfBlocks(function.blocks.size())
    , m_isReachable(m_numberOfBlocks)
    , m_hasHandler(m_numberOfBlocks)
{
    RELEASE_ASSERT(function.hasBody());
    computeReversePostOrder(function);
    computeEdges(function);
    computePredecessors();
    RELEASE_ASSERT(incomingEdges(root()).empty());
}

void CFG::computeReversePostOrder(const Function& function)
{
    m_reversePostOrder.reserveInitialCapacity(m_numberOfBlocks);
    m_isReachable.set(0);
    Vector<std::pair<unsigned, uint32_t>> stack { { 0u, numberOfOutgoingEdges(function.blocks[0]) } };
    while (!stack.isEmpty()) {
        auto [block, numberOfUnvisitedEdges] = stack.last();
        if (!numberOfUnvisitedEdges) {
            m_reversePostOrder.append(BlockID(block));
            stack.removeLast();
            continue;
        }
        uint32_t next = --stack.last().second;
        unsigned target = targetOf(function, outgoingEdgeAt(function, BlockID(block), next)).block.index();
        if (!m_isReachable.set(target))
            stack.append({ target, numberOfOutgoingEdges(function.blocks[target]) });
    }
    m_reversePostOrder.reverse();

    m_reversePostOrderIndex.fill(0, m_numberOfBlocks);
    for (uint32_t orderIndex = 0; orderIndex < m_reversePostOrder.size(); ++orderIndex)
        m_reversePostOrderIndex[m_reversePostOrder[orderIndex].index()] = orderIndex;
}

void CFG::computeEdges(const Function& function)
{
    m_firstSuccessor.fill(0, m_numberOfBlocks + 1);
    m_firstIncomingEdge.fill(0, m_numberOfBlocks + 1);
    for (unsigned block : m_isReachable) {
        m_firstSuccessor[block + 1] = numberOfOutgoingEdges(function.blocks[block]);
        forEachOutgoingEdge(function, BlockID(block), [&](Edge, const Target& target) {
            ++m_firstIncomingEdge[target.block.index() + 1];
        });
        if (function.blocks[block].handler)
            m_hasHandler.set(block);
    }
    for (unsigned block = 0; block < m_numberOfBlocks; ++block) {
        m_firstSuccessor[block + 1] += m_firstSuccessor[block];
        m_firstIncomingEdge[block + 1] += m_firstIncomingEdge[block];
    }

    uint32_t numberOfEdges = m_firstSuccessor.last();
    m_successors.reserveInitialCapacity(numberOfEdges);
    m_incomingEdges.grow(numberOfEdges);
    Vector<uint32_t> nextIncomingEdge = m_firstIncomingEdge;
    for (unsigned block : m_isReachable) {
        forEachOutgoingEdge(function, BlockID(block), [&](Edge edge, const Target& target) {
            m_successors.append(target.block);
            m_incomingEdges[nextIncomingEdge[target.block.index()]++] = edge;
        });
    }
}

void CFG::computePredecessors()
{
    m_predecessors.reserveInitialCapacity(m_incomingEdges.size());
    m_firstPredecessor.reserveInitialCapacity(m_numberOfBlocks + 1);
    m_firstPredecessor.append(0);
    for (unsigned block = 0; block < m_numberOfBlocks; ++block) {
        BlockID previous;
        for (Edge edge : incomingEdges(BlockID(block))) {
            if (edge.from != previous)
                m_predecessors.append(edge.from);
            previous = edge.from;
        }
        m_firstPredecessor.append(m_predecessors.size());
    }
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
