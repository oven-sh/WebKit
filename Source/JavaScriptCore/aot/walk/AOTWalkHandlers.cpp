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
    size_t frameIndex = numberOfFramesUpToHandlerInForce();
    if (m_numberOfFramesOverHandlersOfCode && handlerOfCodeInForce(frameIndex))
        return;
    if (!frameIndex--)
        return;

    Frame& frame = m_frames[frameIndex];
    BodyTarget target = frame.index->block(frame.block).handler();
    WalkState* known = frame.findStateOfHandler(target.block());
    if (!known) {
        frame.addStateOfHandler(target.block(), WalkState());
        return;
    }

    WalkState& state = *known;
    if (state.isEmpty())
        return;
    state.meetWith(m_state);
    for (Reference reference : thrown) {
        if (reference)
            letLeave(state, reference);
    }
    for (ValueID argument : target.arguments()) {
        if (Reference reference = referenceBoundIn(frame, argument))
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
