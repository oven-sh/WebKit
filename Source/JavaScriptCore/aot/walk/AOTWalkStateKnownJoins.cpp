/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// What KnownJoin asks of two states that are about to be met: the places that they do not share, which costs what either state has written since the two were one, and the places of one holder.
// At a suspension and at a throw: nothing, these only read, and a note goes through the notes that WalkState has. Withheld: nobody asks.

#include "config.h"
#include "AOTWalkState.h"

#if ENABLE(AOT)

namespace JSC { namespace AOT { namespace Typed {

Reference WalkState::valueAt(const NameOfPlace& place) const
{
    static_assert(static_cast<uint8_t>(NameOfPlace::Kind::Slot) == static_cast<uint8_t>(PlaceKind::Slot));
    static_assert(static_cast<uint8_t>(NameOfPlace::Kind::Variable) == static_cast<uint8_t>(PlaceKind::Variable));
    static_assert(static_cast<uint8_t>(NameOfPlace::Kind::EnvironmentField) == static_cast<uint8_t>(PlaceKind::EnvironmentField));
    static_assert(static_cast<uint8_t>(NameOfPlace::Kind::Element) == static_cast<uint8_t>(PlaceKind::Element));
    static_assert(static_cast<uint8_t>(NameOfPlace::Kind::GuardOfStores) == static_cast<uint8_t>(PlaceKind::GuardOfStores));
    static_assert(static_cast<uint8_t>(NameOfPlace::Kind::GuardOfReads) == static_cast<uint8_t>(PlaceKind::GuardOfReads));
    return valueOfPlace(static_cast<PlaceKind>(place.kind), place.holder, place.first, place.second);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
