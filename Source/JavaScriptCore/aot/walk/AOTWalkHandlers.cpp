/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

void Walker::noteStateAtThrowForHandler(std::span<const Reference> thrown)
{
    if (handlerMadeOverCopiesIsInForce())
        return;
    size_t frameIndex = numberOfFramesUpToHandlerInForce();
    if (m_numberOfFramesOverHandlersOfCode && handlerOfCodeInForce(frameIndex))
        return;
    if (!frameIndex--)
        return;

    Frame& frame = m_frames[frameIndex];
    BodyTarget target = frame.index->block(frame.block).handler();
    keepStateOfHandlerForUndo(frameIndex, target.block());
    StateOfHandler* known = frame.findStateOfHandler(target.block());
    if (!known) {
        frame.addStateOfHandler(target.block(), WalkState());
        frame.statesOfHandlers.last().serialLogged = m_serialOfNewestMark;
        return;
    }

    WalkState& state = known->state;
    if (state.isEmpty())
        return;
    state.meetWith(m_state, &known->lastMet);
    for (Reference reference : thrown) {
        if (reference)
            letLeave(state, reference);
    }
    for (ValueID argument : target.arguments()) {
        if (Reference reference = nameBehindStandIn(frameIndex, argument, referenceBoundIn(frame, argument)))
            letLeave(state, reference);
    }
}

void enterHandler(Walker& walker, BlockID inputHandler)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (!walker.has(Mechanism::Handlers) || index.hasIrreducibleFlow() || index.isLoopHead(inputHandler))
        walker.state().clear();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
