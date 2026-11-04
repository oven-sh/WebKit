/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// At a throw: a handler starts from the meet of the states at the heads of the blocks under it and behind each operation and terminator under it that may throw, what that writes ended.
// What is thrown and what the handler edge hands on is fresh no longer. No operation of the input is read for it.
// At a suspension under a handler: as at a call of code that nobody has seen.
// Withheld: the state of a handler is empty.

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

// A block of a callee without a handler of its own stands under the handler of the call, as in leadHandlerEdge().
void Walker::noteStateAtThrowForHandler(std::span<const Reference> thrown)
{
    size_t frameIndex = numberOfFramesUpToHandlerInForce();
    if (!frameIndex--)
        return;

    Frame& frame = m_frames[frameIndex];
    BodyTarget target = frame.index->block(frame.block).handler();
    WalkState* known = frame.findStateOfHandler(target.block());
    // The head of the block was not noted, so nothing is known to hold at all of it.
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
        if (Reference reference = frame.references[argument.index()])
            letLeave(state, reference);
    }
}

// On entry Walker::state() is that meet. A block that throws here and is walked behind the handler has no part in it.
void enterHandler(Walker& walker, BlockID inputHandler)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (!walker.has(Mechanism::Handlers) || index.hasIrreducibleFlow() || index.isLoopHead(inputHandler))
        walker.state().clear();
    enterHandlerWithWhatIsKnown(walker, inputHandler);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
