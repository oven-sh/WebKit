/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// `try { walk(root) } catch (e) { if (e instanceof Stop) return e.node; throw e }`: what is thrown goes to whoever catches it. A Join stands for what a handler block catches.
// At a throw: the operand flows into the Join of the handler of its block, else into the Thrown node, and by itself into node 0 from neither. At a suspension: what a resumption throws is what nobody knows, which lies in every Join.
// Withheld (Handlers): the summary is the same. Withheld or not, Link lets every Thrown node flow into unseen code: StateOfLinkReach::bringAlive().

#pragma once

#if ENABLE(AOT)

// For the foot of AOTScan.cpp alone, which declares ScanOfBody.

namespace JSC { namespace AOT { namespace Typed {

inline void ScanOfBody::noteHandlerOf(const BodyBlock& block)
{
    if (!block.hasHandler())
        return;
    if (m_handlersOfBlocks.isEmpty())
        m_handlersOfBlocks.fill(BlockID(), m_function->body.numberOfBlocks());
    BlockID handler = block.handler().block();
    m_handlersOfBlocks[block.id().index()] = handler;
    nodeCaughtBy(handler);
}

inline uint32_t ScanOfBody::nodeCaughtBy(BlockID handler)
{
    if (m_nodesCaughtByBlocks.isEmpty())
        m_nodesCaughtByBlocks.fill(0, m_function->body.numberOfBlocks());
    if (!m_nodesCaughtByBlocks[handler.index()]) {
        uint32_t caught = newJoin();
        m_nodesCaughtByBlocks[handler.index()] = caught;
        m_summary.handlers.append(SummaryHandler { handler, caught });
    }
    return m_nodesCaughtByBlocks[handler.index()];
}

inline void ScanOfBody::settleWhereRecordsThrow()
{
    if (m_summary.handlers.isEmpty())
        return;

    // A record without a site belongs to a return of a coroutine, which no handler of the body stands around.
    auto nodeTakingWhatIsThrownAt = [&](ValueID site) {
        BlockID handler = site && !m_handlersOfBlocks.isEmpty() ? m_handlersOfBlocks[m_blocks[site.index()]] : BlockID();
        return handler ? m_nodesCaughtByBlocks[handler.index()] : noSummaryNode;
    };
    for (SummaryCall& call : m_summary.calls)
        call.thrownTo = nodeTakingWhatIsThrownAt(call.site);
    for (SummaryAccess& access : m_summary.accesses)
        access.thrownTo = nodeTakingWhatIsThrownAt(access.site);

    // The engine throws errors of its own wherever it runs out of stack or of memory, under a call by binding too, of which ownEffects says nothing.
    for (const SummaryHandler& handler : m_summary.handlers)
        addEdge(0, handler.caught);
    std::ranges::sort(m_summary.handlers, { }, &SummaryHandler::handler);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
