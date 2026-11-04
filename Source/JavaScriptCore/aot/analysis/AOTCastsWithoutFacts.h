/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include "AOTVector.h"

namespace JSC { namespace AOT { namespace Typed {

// A cast that no test backs says something of its value. Without a fact it says so wherever the value is at hand, so it stands where that is plain: in the block that defines the value, close behind
// the definition, with no operation that may leave the function between the two. Anywhere else it says what holds from a place on, and names a fact that is made there: whoever looks at the value through the cast
// names the cast, the cast names the fact, and so nobody puts the look in front of that place.
struct CastAwayFromItsValue {
    BlockID block;
    uint32_t indexInBlock;
    ValueID cast;
};
// Those of one block follow one another, in the order in which they stand.
// A cast of a word that may be anything is none of them: it hands the word on, nobody looks at what it gives, and with a fact it would say that the word is a value.
Vector<CastAwayFromItsValue, 4> findCastsAwayFromTheirValues(const Program&, const Function&);

// For whoever has put another value in place of the value of a cast, has cut the code between the two, or has made a cast where its value is used, and has moved no cast to where the program had none:
// each such cast holds where it stands. Answers with the blocks that got a fact.
Vector<BlockID, 4> tieCastsWhereTheyStand(const Program&, Function&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
