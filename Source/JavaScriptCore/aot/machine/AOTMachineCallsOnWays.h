/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTMachineAllocator.h"

namespace JSC { namespace AOT { namespace Machine {

bool callStandsOnWayFromDefinitionToRead(Allocator&, const Operation* value);
bool callStandsOnWayFromHeadToRead(Allocator&, const Operation* value);
bool callStandsOnWayFromPhiToRead(Allocator&, const Operation* phi, const Block*);

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
