/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Handlers, Guesses: `catch (e) { return e.node }`. What was seen to arrive at a handler is expected of what it catches. No proof in either reading: an error of the engine's own, a host function and a rejection bring what nobody lists.
// At a throw and at a suspension: nothing is built here. Withheld: the table is empty, which claims nothing.

#pragma once

#if ENABLE(AOT)

// For the foot of AOTLink.cpp alone, which declares WriterOfLink.

namespace JSC { namespace AOT { namespace Typed {

// The functions come in ascending order, and so do the handlers of a summary.
inline void WriterOfLink::writeCatches()
{
    if (!m_flow.mechanisms.contains(Mechanism::Handlers))
        return;
    for (uint32_t index = 0; index < m_numberOfFunctions; ++index) {
        if (!m_flow.reached.get(index))
            continue;
        FunctionID id(index);
        for (const SummaryHandler& handler : m_flow.summaries.at(id).handlers) {
            const LinkType arrived = m_flow.withoutObjectsThatHaveEscaped(m_flow.typeAt(m_flow.nodeOf(id, handler.caught)));
            LinkType seen = arrived;
            seen.holdsUnseen = false;
            seen.holdsLostBuiltins = false;
            if (TypeID caught = withoutLostBuiltinsInLink(arrived).isAnything() ? TypeID() : typeIDOf(seen))
                m_tables.catches.append(LinkedCatch { id, handler.handler, caught });
        }
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
