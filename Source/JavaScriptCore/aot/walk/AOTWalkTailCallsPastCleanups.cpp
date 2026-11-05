/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTOpcodeSets.h"

namespace JSC { namespace AOT { namespace Typed {

ValueID openOfCleanupThatOnlyThrowsOn(const BodyIndex& index, BodyBlock block)
{
    BodyTarget edge = block.handler();
    if (!edge.arguments().empty())
        return { };
    BodyBlock cleanup = index.block(edge.block());
    if (cleanup.role() != BlockRole::IterationCleanup || cleanup.numberOfParameters() || cleanup.hasHandler())
        return { };

    ValueID open = cleanup.subject();
    if (!open || index.isParameter(open) || index.opcodeOf(open) != Opcode::IteratorOpen)
        return { };
    ValueID caught;
    bool closes = false;
    for (BodyValue operation : cleanup.values()) {
        if (!caught) {
            if (!catchOpcodes.contains(operation.opcode()))
                return { };
            caught = operation.id();
            continue;
        }
        if (operation.opcode() == Opcode::ReleaseHint)
            continue;
        if (closes || operation.opcode() != Opcode::IteratorClose || operation.operand(0) != open || operation.as<Opcode::IteratorClose>().completion() != IteratorCompletion::Throw)
            return { };
        closes = true;
    }
    BodyTerminator end = cleanup.terminator();
    return closes && end.kind() == TerminatorKind::Rethrow && end.operand(0) == caught ? open : ValueID();
}

bool Walker::throwsPastHandlerOf(BodyBlock block) const
{
    if (!has(Mechanism::TypedOperations))
        return false;
    const Frame& frame = m_frames.last();
    ValueID open = openOfCleanupThatOnlyThrowsOn(*frame.index, block);
    if (!open)
        return false;

    Reference state = referenceBoundIn(frame, open);
    if (!state || !state.isVirtual())
        return false;
    const IterationByIndex& iteration = m_iterationsByIndex[state.object().index()];
    return iteration.list && !iteration.iterator && !iteration.mayGoByProtocol && iteration.inlineFrame == frame.inlineFrame;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
