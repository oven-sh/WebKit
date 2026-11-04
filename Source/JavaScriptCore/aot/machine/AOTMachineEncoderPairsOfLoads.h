/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTMachineEncoder.h"

namespace JSC { namespace AOT { namespace Machine {

// ARM64: two Loads of one width and bank through one base register from neighboring words, whose steps follow each other. True: one instruction has been written for both.
// The caller stores what is spilled of the two behind it.
bool tryEncodePairOfLoads(Encoder&, const Step& first, const Step& second);

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
