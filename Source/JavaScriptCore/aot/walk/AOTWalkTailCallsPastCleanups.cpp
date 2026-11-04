/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// `for (const x of list) f(x)`: the iteration goes by index, so its close builds nothing, and the cleanup block does nothing but throw on. The blocks of the loop lead no edge there.
// At a suspension: nothing. An iteration by index is synchronous.
// At a throw: it goes where the cleanup block would have thrown it on to: to the handler of the call around a copy, or out of the unit.
// Withheld, with TypedOperations: no iteration goes by index, and every block stands under the handler that it names.

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTOpcodeSets.h"

namespace JSC { namespace AOT { namespace Typed {

// Of a block of the body being read that names a handler.
bool Walker::throwsPastHandlerOf(BodyBlock block) const
{
    if (!has(Mechanism::TypedOperations))
        return false;
    const Frame& frame = m_frames.last();
    BodyTarget edge = block.handler();
    if (!edge.arguments().empty())
        return false;
    BodyBlock cleanup = frame.index->block(edge.block());
    if (cleanup.role() != BlockRole::IterationCleanup || cleanup.numberOfParameters() || cleanup.hasHandler())
        return false;

    // What it catches, one close of its iteration, and hints, which no run can tell.
    ValueID open = cleanup.subject();
    if (!open || frame.index->isParameter(open) || frame.index->opcodeOf(open) != Opcode::IteratorOpen)
        return false;
    ValueID caught;
    bool closes = false;
    for (BodyValue operation : cleanup.values()) {
        if (!caught) {
            if (!catchOpcodes.contains(operation.opcode()))
                return false;
            caught = operation.id();
            continue;
        }
        if (operation.opcode() == Opcode::ReleaseHint)
            continue;
        if (closes || operation.opcode() != Opcode::IteratorClose || operation.operand(0) != open || operation.as<Opcode::IteratorClose>().completion() != IteratorCompletion::Throw)
            return false;
        closes = true;
    }
    BodyTerminator end = cleanup.terminator();
    if (!closes || end.kind() != TerminatorKind::Rethrow || end.operand(0) != caught)
        return false;

    // No iterator was made, so no `return` can be found: BuilderOfIteration::closeByIndex().
    Reference state = frame.references[open.index()];
    if (!state || !state.isVirtual())
        return false;
    const IterationByIndex& iteration = m_iterationsByIndex[state.object().index()];
    return iteration.list && !iteration.iterator && !iteration.mayGoByProtocol && iteration.inlineFrame == frame.inlineFrame;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
