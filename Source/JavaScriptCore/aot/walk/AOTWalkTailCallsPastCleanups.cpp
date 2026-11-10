/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTOpcodeSets.h"

namespace JSC { namespace AOT { namespace Typed {

static ValueID iteratorClosedByHandler(const BodyIndex& index, BodyBlock block)
{
    BodyTarget edge = block.handler();
    if (!edge.arguments().empty())
        return { };
    BodyBlock cleanup = index.block(edge.block());
    if (cleanup.role() != BlockRole::IterationCleanup || cleanup.numberOfParameters())
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
        if (closes || operation.opcode() != Opcode::IteratorClose || operation.operand(0) != open || operation.as<Opcode::IteratorClose>().completion() != IteratorCompletion::Throw)
            return { };
        closes = true;
    }
    BodyTerminator end = cleanup.terminator();
    return closes && end.kind() == TerminatorKind::Rethrow && end.operand(0) == caught ? open : ValueID();
}

ValueID iteratorClosedLeavingFunction(const BodyIndex& index, BodyBlock block)
{
    return index.block(block.handler().block()).hasHandler() ? ValueID() : iteratorClosedByHandler(index, block);
}

ValueID iteratorClosedLeavingLoop(const BodyIndex& index, BodyBlock block, LoopID loop)
{
    BodyBlock cleanup = index.block(block.handler().block());
    if (cleanup.hasHandler() && index.isInLoop(cleanup.handler().block(), loop))
        return { };
    return iteratorClosedByHandler(index, block);
}

bool Walker::handlerOnlyClosesIterations(const BodyIndex& index, BodyBlock block) const
{
    for (BodyBlock covered = block;;) {
        if (!iteratorClosedByHandler(index, covered))
            return false;
        BodyBlock cleanup = index.block(covered.handler().block());
        if (!cleanup.hasHandler())
            return true;
        covered = cleanup;
    }
}

bool Walker::throwsPastHandlerOf(BodyBlock block) const
{
    if (!has(Mechanism::TypedOperations))
        return false;
    const Frame& frame = m_frames.last();
    const BodyIndex& index = *frame.index;
    for (BodyBlock covered = block;;) {
        ValueID open = iteratorClosedByHandler(index, covered);
        Reference state = open ? referenceBoundIn(frame, open) : Reference();
        if (!state || !state.isVirtual())
            return false;
        const IndexedIteration& iteration = m_indexedIterations[state.object().index()];
        if (!iteration.list || iteration.iterator || iteration.hasProtocolFallback || iteration.inlineFrame != frame.inlineFrame)
            return false;
        BodyBlock cleanup = index.block(covered.handler().block());
        if (!cleanup.hasHandler())
            return true;
        covered = cleanup;
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
