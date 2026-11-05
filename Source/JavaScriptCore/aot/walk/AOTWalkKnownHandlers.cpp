/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Handlers: what a handler starts from. A cell that the state holds as the content of a place, and that nothing in the handler names, is read again there:
// for the handler alone it would stay in the frame or in a saved register across every call under the handler, and the handler is cold.
// At a suspension: nothing of its own. At a throw: this is where the throw arrives. It only ends what is known.
// Withheld: Walker::enterHandlerBlock() has emptied the state.

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTSweep.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT { namespace Typed {

// The places of fresh objects stay: nothing reads them from memory, and the sweep may still drop the object.
void WalkState::endPlacesHolding(NOESCAPE const ScopedLambda<bool(Reference value)>& isEnded)
{
    if (m_held && m_held->m_numberOfPlaces)
        heldToWrite().endPlacesHolding(isEnded);
}

void WalkState::Held::endPlacesHolding(NOESCAPE const ScopedLambda<bool(Reference value)>& isEnded)
{
    for (uint32_t holder : m_holdersReachedByEffects) {
        for (uint32_t first : { m_holders[holder].firstPlace, m_holders[holder].firstElement }) {
            // A list may still hold places that have ended.
            for (uint32_t position = first; position != none; position = m_places[position].next) {
                Reference value = m_places[position].value;
                if (value && isEnded(value))
                    endPlace(position);
            }
        }
    }
    dropEndedRecordsIfMany();
}

void enterHandlerWithWhatIsKnown(Walker& walker, BlockID inputHandler)
{
    WalkState& state = walker.state();
    if (!walker.has(Mechanism::Handlers) || state.isEmpty())
        return;

    // What the block is entered with: whatever is live at its head as the program is written. A conversion is no name of what it converts, and holds the same cell.
    const Code& code = walker.code();
    const BodyIndex& index = walker.currentBodyIndex();
    Vector<uint32_t, 32> rootsAtHand;
    auto noteAtHand = [&](ValueID input) {
        Reference reference = walker.referenceBoundTo(input);
        if (reference && reference.isValue())
            rootsAtHand.append(rootOfCellInSweptCode(code, reference.id()).index());
    };
    // TODO: what is live at the head is at hand too. What is live at every head of a body grows faster than the body, so it is not asked for: more ends here than has to.
    const auto& block = index.block(inputHandler);
    for (uint32_t parameter = 0; parameter < block.numberOfParameters(); ++parameter)
        noteAtHand(block.parameter(parameter));
    std::ranges::sort(rootsAtHand);

    TypeArena& types = walker.types();
    state.endPlacesHolding(scopedLambda<bool(Reference)>([&](Reference value) {
        if (!value.isValue())
            return false;
        ValueID root = rootOfCellInSweptCode(code, value.id());
        TypeID typeOfRoot = code.isConstant(root) ? TypeID() : code.at(root).type;
        if (!typeOfRoot)
            return false;
        Type type = types.at(typeOfRoot);
        bool mayBeCell = type.hasObjects() || type.environment || type.opaque;
        // A value that a test has narrowed says more than a new read would.
        if (!mayBeCell || walker.typeOf(value) != typeOfRoot)
            return false;
        return !std::ranges::binary_search(rootsAtHand, root.index());
    }));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
