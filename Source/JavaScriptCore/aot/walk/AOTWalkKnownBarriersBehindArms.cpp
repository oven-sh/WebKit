/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// TypedOperations: where arms meet that each store into one object, one RememberOwner behind them runs the barrier for all. The walk only proposes: the sweep marks the stores that it covers, and drops it where none waits.
// At a suspension, and at whatever else may collect: what the state knows of stores ends, so nothing is proposed behind it.
// At a throw: the state of a handler knows of no store, so a way out of a handler that meets the arms brings none and nothing is proposed.
// Withheld: no store is noted, and every store runs its own barrier.

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTOpcodeSets.h"
#include "AOTOperandKinds.h"
#include "AOTSweep.h"
#include "AOTTypeArena.h"

namespace JSC { namespace AOT { namespace Typed {

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
