/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTMachineEncoder.h"

namespace JSC { namespace AOT { namespace Machine {

bool tryEncodePairOfLoads(Encoder&, const Step& first, const Step& second);

} } } // namespace JSC::AOT::Machine

#endif // ENABLE(AOT)
