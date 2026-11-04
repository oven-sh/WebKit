/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// EscapeAnalysis: a store into a cell that was made behind the last safepoint, and that nothing else names yet, runs no barrier: the collector cannot have marked the cell.
// At a suspension: whatever suspends runs other code, so no cell is young behind it.
// At a throw: the error is made on the way to the handler, so no cell is young at the head of a handler block: KnownBarriers::enterHead().
// Withheld: no cell is noted, and every store keeps its barrier.

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTOpcodeSets.h"
#include "AOTOperandKinds.h"
#include "AOTPlan.h"
#include "AOTSweep.h"
#include "AOTTypeArena.h"

namespace JSC { namespace AOT { namespace Typed {

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
