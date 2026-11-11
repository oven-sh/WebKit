/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

inline void WriterOfLink::writeCatches()
{
    if (!m_flow.mechanisms.contains(Mechanism::Handlers))
        return;
    struct Handler {
        FunctionID function;
        BlockID handler;
        FlowNodeID caught;
    };
    Vector<Handler> found = collectInParallel<Handler>(loops(), m_numberOfFunctions, [&](unsigned index, Vector<Handler>& handlersOfChunk) {
        if (!m_flow.reached.get(index))
            return;
        FunctionID id(index);
        for (const SummaryHandler& handler : m_flow.summaries.at(id).handlers)
            handlersOfChunk.append(Handler { id, handler.handler, m_flow.nodeOf(id, handler.caught) });
    });
    for (const Handler& handler : found) {
        const LinkType arrived = m_flow.withoutObjectsThatHaveEscaped(m_flow.typeAt(handler.caught));
        LinkType seen = arrived;
        seen.hasUnknownObjects = false;
        seen.holdsLostBuiltins = false;
        if (TypeID caught = withoutLostBuiltinsInLink(arrived).isAnything() ? TypeID() : typeIDOf(seen))
            m_tables.catches.append(LinkedCatch { handler.function, handler.handler, caught });
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
