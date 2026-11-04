/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTWalk.h"

namespace JSC { namespace AOT { namespace Typed {

// `while (index < end) { .. index++; }`, where nobody has typed what hands in the position and the end: every turn compares and steps two values of which nothing is known, by stub.
// What a turn only steps by whole numbers is a counter. What enters the head for it is asked ONCE, in front of the loop, whether it is an int32, and so is what it is compared with.
// Where all pass, the loop is walked with that proven of what enters, and the looks at that copy find what the steps make of it. Where one fails, the closed copy of the loop runs: Walker::buildLoopTwice().
// No proof, and nothing is claimed of the program: a text, a fraction, an object with a valueOf() take the closed copy, which does what the language says.
struct CounterTestedInFrontOfLoop {
    // Among the parameters of the head.
    uint32_t position { 0 };
    TypeID tested;
};

// For Walker::buildLoopTwice(), behind findGuessesInFrontOfLoop(), while the parameters of the head are bound to what enters. `guesses` gains the bounds that no guess is listed for.
Vector<CounterTestedInFrontOfLoop, 2> findCountersTestedInFrontOfLoop(Walker&, BlockID inputHead, std::span<const Reference> entering, Vector<GuessInFrontOfLoop, 4>& guesses);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
