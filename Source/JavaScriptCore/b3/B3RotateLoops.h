/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT) && USE(BUN_JSC_ADDITIONS)

#include <wtf/Vector.h>

namespace JSC { namespace B3 {

class BasicBlock;
class Procedure;

// Turns a loop that is tested at the top into a guard and a loop that is tested at the bottom: the blocks from the header to the test are copied
// for the back edges that are not rare. The originals stay as the guard. The body's first block becomes the header, with a pre-header of its own
// behind the guard: what is moved there runs only if the body does. Where it yields true it leaves variables: fixSSA() has to follow.
bool rotateLoops(Procedure&, const Vector<BasicBlock*>* onlyWithTheseHeaders = nullptr);

// A Branch on a constant, as foldPathConstants() leaves it, becomes a Jump, and what is no longer reached goes.
bool simplifyTerminals(Procedure&);

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT) && USE(BUN_JSC_ADDITIONS)
