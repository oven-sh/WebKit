/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(B3_JIT) && USE(BUN_JSC_ADDITIONS)

namespace JSC { namespace B3 {

class Procedure;

// Makes a fast version of an innermost loop out of its blocks that are not rare. A rare edge out of the fast version goes to the block it went to, which goes on
// in the original loop: nothing comes back. So in the fast version nothing is written but what its own stores write. What is loop invariant there, along the blocks
// that are guaranteed to execute, goes to its pre-header: pure values, loads, and guards (a two-way terminal with a successor that is rare or leaves the loop), which enter the original loop where
// they fail. A control-dependent value goes once every guard in front of it has gone. Where it yields true it leaves variables: fixSSA() has to follow.
bool versionLoops(Procedure&);

} } // namespace JSC::B3

#endif // ENABLE(B3_JIT) && USE(BUN_JSC_ADDITIONS)
