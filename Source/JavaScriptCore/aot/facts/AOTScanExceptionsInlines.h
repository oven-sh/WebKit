/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

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

    auto nodeTakingWhatIsThrownAt = [&](ValueID site) {
        BlockID handler = site && !m_handlersOfBlocks.isEmpty() ? m_handlersOfBlocks[m_blocks[site.index()]] : BlockID();
        return handler ? m_nodesCaughtByBlocks[handler.index()] : noSummaryNode;
    };
    for (SummaryCall& call : m_summary.calls)
        call.thrownTo = nodeTakingWhatIsThrownAt(call.site);
    for (SummaryAccess& access : m_summary.accesses)
        access.thrownTo = nodeTakingWhatIsThrownAt(access.site);

    for (const SummaryHandler& handler : m_summary.handlers)
        addEdge(0, handler.caught);
    std::ranges::sort(m_summary.handlers, { }, &SummaryHandler::handler);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
