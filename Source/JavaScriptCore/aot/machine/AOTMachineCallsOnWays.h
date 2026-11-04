/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTMachineAllocator.h"

namespace JSC { namespace AOT { namespace Machine {

// Whether a value needs a register that calls keep. Neither answer is a proof: a false one puts the decision off to the call that forces it.
// The scan's half is members of BackwardScan, in AOTMachineCallsOnWaysInScan.h: noteWaysOnFrom().
// One answer of the scan is a proof, and Allocator::isDeadOnEveryPathFrom() goes by it: Liveness::endOfWhatWaysLeadToFrom().

// Of a value with ValueFlag::LivesAcrossCall, which goes by the order: the arms of a Branch stand one behind the other there, so what a later arm reads counts as live across the calls of every earlier one.
// False: no way leads from a call behind the definition to a read of the value. True where that is not known.
// Whoever goes by a false answer leaves the value in any register and stores nothing. Where a call finds it bound all the same, vacate() moves it or evict() spills it, as any other value.
// The value is being defined in the block of the walk.
bool callStandsOnWayFromDefinitionToRead(Allocator&, const Operation* value);
// The value is bound at the head of the block of the walk, which stands outside rare regions.
bool callStandsOnWayFromHeadToRead(Allocator&, const Operation* value);
// The value is a Phi of the block, which stands outside rare regions and behind the block of the walk.
bool callStandsOnWayFromPhiToRead(Allocator&, const Operation* phi, const Block*);

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
